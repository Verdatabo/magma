
/**
 * @file /magma/objects/mail/store_message.c
 *
 * @brief	Functions used to store and copy mail message data.
 */

#include "magma.h"

/**
 * @brief	Persist a message's data to disk.
 * @param	messagenum	the numerical id of the message that will be associated with the data.
 * @param	data		a pointer to a buffer containing the message's data.
 * @param	fflags		the status flags to be stored in the message's on-disk file header.
 * @param	data_len	the size, in bytes, of the message's data.
 * @param	pathptr		if not NULL, the address of a pointer to a string that will receive a copy of the message's on-disk data.
 * @return	true if the storage operation succeeded, or false on failure.
 */
bool_t mail_store_message_data(uint64_t messagenum, uint8_t flags, stringer_t *data, chr_t **pathptr) {

	int_t fd;
	chr_t *path;
	message_header_t header;

	header.magic1 = FMESSAGE_MAGIC_1;
	header.magic2 = FMESSAGE_MAGIC_2;
	header.reserved = 0;
	header.flags = flags;

	// In case of possible failure.
	if (pathptr) {
		*pathptr = NULL;
	}

	// Build the message path.
	if (!(path = mail_message_path(messagenum, NULL))) {
		log_error("Could not build the message path.");
		return false;
	}

	// If we can't open the file, try creating the directory, and then opening the file again.
	if ((fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_SYNC, S_IRUSR | S_IWUSR)) < 0) {

		if (mail_create_directory(messagenum, NULL)) {
			fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_SYNC, S_IRUSR | S_IWUSR);
		}

	}

	// Check and make sure we have a valid file descriptor.
	if (fd < 0) {
		log_error("An error occurred while trying to get a file descriptor.");
		ns_free(path);
		return false;
	}

	// Write the data out to disk, starting with the header.

	if ((write(fd, &header, sizeof(header)) != sizeof(header)) || (write(fd, st_data_get(data), st_length_get(data)) != st_length_get(data))) {
		log_error("Error writing message data to disk. { errno = %i }", errno);
		close(fd);
		unlink(path);
		ns_free(path);
		return false;
	}

	// Flush the buffer.
	if (fsync(fd) != 0) {
		log_error("Could not flush the write buffers to disk. { errno = %i }", errno);
		close(fd);
		unlink(path);
		ns_free(path);
		return false;
	}

	// Close the descriptor.
	if (close(fd) != 0) {
		log_error("An error occurred while trying to close the file descriptor. { errno = %i }", errno);
		unlink(path);
		ns_free(path);
		return false;
	}

	if (pathptr) {
		*pathptr = path;
	}

	return true;
}

/**
 * @brief	Store a mail message, with its meta-information in the database, and the contents persisted to disk.
 * @note	The stored message is always compressed, but only encrypted if the user's public key is suppplied.
 * @param	usernum		the numerical id of the user to which the message belongs.
 * @param	pubkey		if not NULL, a public key that will be used to encrypt the message for the intended user.
 * @param	foldernum	the folder # that will contain the message.
 * @param	status		a pointer to the status flags value for the message, which will be updated if the message is to be encrypted.
 * @param	signum		the spam signature for the message.
 * @param	sigkey		the spam key for the message.
 * @param	message		a managed string containing the raw body of the message.
 * @return	0 on failure, or the newly inserted id of the message in the database on success.
 */
uint64_t mail_store_message(uint64_t usernum, prime_t *signet, uint64_t foldernum, uint32_t *status, uint64_t signum, uint64_t sigkey, stringer_t *message) {

	chr_t *path;
	uint64_t messagenum;
	bool_t store_result;
	compress_t *reduced = NULL;
	stringer_t *encrypted = NULL;
	int64_t transaction = -1, result = 0;
	uint8_t flags = 0;

	// Next, encrypt the message if necessary.
	if (signet) {
		if (!(encrypted = prime_message_encrypt(message, NULL, NULL, org_key, signet))) {
			log_pedantic("Unable to encrypt the email message.");
			return 0;
		}

		flags |= FMESSAGE_OPT_ENCRYPTED;
		*status |= MAIL_STATUS_ENCRYPTED;
	}
	else {

		if (!(reduced = compress_lzo(message))) {
			log_pedantic("Unable to compress the email message.");
			return 0;
		}

		flags |= FMESSAGE_OPT_COMPRESSED;
	}


	// Begin the transaction.
	if ((transaction = tran_start()) < 0) {
		log_error("Could not start a transaction. { transaction = %li }", transaction);
		compress_cleanup(reduced);
		prime_cleanup(encrypted);
		return 0;
	}

	// Insert a record into the database.
	if ((messagenum = mail_db_insert_message(usernum, foldernum, *status, st_length_int(message), signum, sigkey, transaction)) == 0) {
		log_pedantic("Could not create a record in the database. mail_db_insert_message = 0");
		tran_rollback(transaction);
		compress_cleanup(reduced);
		prime_cleanup(encrypted);
		return 0;
	}

	// Now attempt to save everything to disk.
	store_result = mail_store_message_data(messagenum, flags, (encrypted ? encrypted :
		PLACER(compress_body_data(reduced), compress_total_length(reduced))), &path);

	compress_cleanup(reduced);
	st_cleanup(encrypted);

	// If the disk operation failed...
	if (!store_result || !path) {
		log_pedantic("Failed to store the user's message to disk.");
		tran_rollback(transaction);

		if (path) {
			unlink(path);
			ns_free(path);
		}

		return 0;
	}

	// Commit the transaction.
	if ((result = tran_commit(transaction))) {
		log_error("Could not commit the transaction. { commit = %li }", result);
		unlink(path);
		ns_free(path);
		return 0;
	}

	ns_free(path);
	return messagenum;
}

/**
 * @brief	Create a copy of a mail message, with a new entry in the database and a hard link to the message contents on disk.
 * @param	usernum		the numerical id of the user to whom the mail message belongs.
 * @param	original	the numerical id of the mail message to be copied.
 * @param	server		a pointer to a null-terminated string containing the name of the server where the message contents are stored.
 * @param	size		the size, in bytes, of the mail message to be copied.
 * @param	foldernum	the numerical id of the folder to become the parent folder of the message copy.
 * @param	signum		the spam signature for the message.
 * @param	sigkey		the spam key for the message.
 * @param	created		the UNIX timestamp of when the message was created.
 * @return	0 on failure, or the ID of the copy of the mail message in the database on success.
 */
uint64_t mail_copy_message(uint64_t usernum, uint64_t original, chr_t *server, uint32_t size, uint64_t foldernum, uint32_t status, uint64_t signum, uint64_t sigkey, uint64_t created) {

	int_t fd, state;
	uint64_t messagenum;
	int64_t transaction, ret;
	chr_t *origpath, *copypath;

	// Build the original message path.
	if (!(origpath = mail_message_path(original, server))) {
		log_error("Could not build the message path.");
		return 0;
	}

	// Verify the message still exists by opening the file.
	if ((fd = open(origpath, O_RDONLY)) < 0) {
		log_pedantic("Could not open a file descriptor for the message %s.", origpath);
		ns_free(origpath);
		return 0;
	}

	close(fd);

	// Begin the transaction.
	if ((transaction = tran_start()) < 0) {
		log_error("Could not start a transaction. {start = %li}", transaction);
		ns_free(origpath);
		return 0;
	}

	// Insert a record into the database.
	if (!(messagenum = mail_db_insert_duplicate_message(usernum, foldernum, status, size, signum, sigkey, created, transaction))) {
		log_pedantic("Could not create a record in the database. mail_db_insert_message = 0");
		tran_rollback(transaction);
		ns_free(origpath);
		return 0;
	}

	// Build the message path.
	if (!(copypath = mail_message_path(messagenum, NULL))) {
		log_error("Could not build the message path.");
		tran_rollback(transaction);
		ns_free(origpath);
		return 0;
	}

	// Create a hard link between the old message path and the new one.
	if ((state = link(origpath, copypath)) != 0) {

		if (mail_create_directory(messagenum, NULL)) {
			state = link(origpath, copypath);
		}

	}

	// Make sure the link was created.
	if (state != 0) {
		log_error("Could not create a hard link between two messages. link = %i", state);
		tran_rollback(transaction);
		ns_free(origpath);
		ns_free(copypath);
		return 0;
	}

	// Commit the transaction.
	if ((ret = tran_commit(transaction))) {
		log_error("Could not commit the transaction. { commit = %li }", ret);
		ns_free(origpath);
		ns_free(copypath);
		return 0;
	}

	ns_free(origpath);
	ns_free(copypath);

	return messagenum;
}

/**
 * @brief	Move a message to a new folder in the database.
 * @param	usernum		the numerical id of the user that owns the message.
 * @param	messagenum	the numerical id of the message to be moved.
 * @param	source		the numerical id of the current parent folder of the specified message.
 * @param	target		the numerical id of the folder to which the specified message will be moved.
 * @return	-1 on error, 0 if the message wasn't found, or 1 on success.
 */
int_t mail_move_message(uint64_t usernum, uint64_t messagenum, uint64_t source, uint64_t target) {

	int_t result;
	int64_t transaction;

	// Begin the transaction.
	if ((transaction = tran_start()) < 0) {
		log_error("Could not start a transaction. {start = %li}", transaction);
		return -1;
	}

	// Insert a record into the database.
	if ((result = mail_db_update_message_folder(usernum, messagenum, source, target, transaction)) != 1) {
		log_pedantic("Could not move a message between folders. { mail_db_update_message_folder = %i }", result);
		tran_rollback(transaction);
		return result;
	}

	// Commit the transaction.
	if ((result = tran_commit(transaction))) {
		log_error("Could not commit message move transaction. { commit = %i }", result);
		return -1;
	}

	return 1;
}
