/*
	Copyright 2009 Vincent Duvert, vincent.duvert@free.fr
	All rights reserved. Distributed under the terms of the MIT License.
*/

#include "VMWSharedFolders.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define ASSERT(x) if (!(x)) panic("ASSERT FAILED : " #x);
#define CALLED() dprintf("vmwfs: %s was called.\n", __FUNCTION__)

#include <KernelExport.h>

enum {
	VMW_CMD_OPEN_FILE = 0,
	VMW_CMD_READ_FILE,
	VMW_CMD_WRITE_FILE,
	VMW_CMD_CLOSE_FILE,
	VMW_CMD_OPEN_DIR,
	VMW_CMD_READ_DIR,
	VMW_CMD_CLOSE_DIR,
	VMW_CMD_GET_ATTR,
	VMW_CMD_SET_ATTR,
	VMW_CMD_NEW_DIR,
	VMW_CMD_DEL_FILE,
	VMW_CMD_DEL_DIR,
	VMW_CMD_MOVE_FILE
};
	
#define SET_8(var, offset, val) *(uint8*)((var) + offset) = (uint8)val; offset += 1
#define SET_32(var, offset, val) *(uint32*)((var) + offset) = (uint32)val; offset += 4
#define SET_64(var, offset, val) *(uint64*)((var) + offset) = (uint64)val; offset += 8

#define SIZE_8 1
#define SIZE_32 4
#define SIZE_64 8
#define SIZE_START 6

VMWSharedFolders::VMWSharedFolders()
{
	CALLED();
	if (!backdoor.InVMware() || backdoor.OpenRPCChannel() != B_OK) {
		init_check = B_ERROR;
		return;
	}
	
	init_check = backdoor.SendMessage("f ", true);
}

VMWSharedFolders::~VMWSharedFolders()
{
	CALLED();
	backdoor.CloseRPCChannel();
}

status_t
VMWSharedFolders::OpenFile(const char* path, int open_mode, file_handle* handle)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Access mode : 0 => RO, 1 => WO, 2 => RW (32-bits, in BuildCommand)
	// 3) Open mode (32-bits)
	// 4) Permissions : 1 => exec, 2 => write, 4 => read (8-bits)
	// 5) The path length, with ending null (32-bits)
	// 6) The path itself (with / path delimiters replaced by null characters)
	
	const size_t path_length = strlen(path);
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + SIZE_32 + SIZE_8 + SIZE_32 + path_length + 1;
	
	char* command = (char*)malloc(cmd_length);
	
	off_t pos = BuildCommand(command, VMW_CMD_OPEN_FILE, open_mode & 3);

	uint32 vmw_openmode;
	if (open_mode & O_TRUNC == O_TRUNC) {
		vmw_openmode = 0x04;
	} else if (open_mode & O_CREAT == O_CREAT) {
		if (open_mode & O_EXCL == O_EXCL)
			vmw_openmode = 0x03;
		else
			vmw_openmode = 0x02;
	} else {
		vmw_openmode = 0;
	}
	
	SET_32(command, pos, vmw_openmode);
	SET_8(command, pos, 0x06);
	SET_32(command, pos, path_length);
	CopyPath(path, command + pos, &pos);
	
	ASSERT(pos == cmd_length);
	
	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);
	
	if (ret != B_OK)
		return ret;
	
	size_t length;
	char* received = backdoor.GetMessage(&length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (length != 14) {
		free(received);
		return B_ERROR;
	}

	ret = ConvertStatus(*(uint32*)(received + 6));
	*handle = *(uint32*)(received + 10);
	
	free(received);
	
	return ret;
}

status_t
VMWSharedFolders::ReadFile(file_handle handle, uint64 offset, void* buffer, uint32* length)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Handle (32-bits, in BuildCommand)
	// 3) Offset (64-bits)
	// 4) Length (32-bits)
	
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + SIZE_64 + SIZE_32;
	
	char* command = (char*)malloc(cmd_length);
	off_t pos = BuildCommand(command, VMW_CMD_READ_FILE, handle);
	SET_64(command, pos, offset);
	SET_32(command, pos, *length);

	ASSERT(pos == cmd_length);

	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);

	if (ret != B_OK)
		return ret;
	
	size_t msg_length;
	char* received = backdoor.GetMessage(&msg_length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (msg_length < 14) {
		free(received);
		return B_ERROR;
	}
	
	ret = ConvertStatus(*(uint32*)(received + 6));
	*length = *(uint32*)(received + 10);
	
	memcpy(buffer, received + 14, *length);
	free(received);
	
	return B_OK;
}

status_t
VMWSharedFolders::WriteFile(file_handle handle, uint64 offset, const void* buffer, uint32* length)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Handle (32-bits, in BuildCommand)
	// 3) ???? (8-bits)
	// 4) Offset (64-bits)
	// 5) Length (32-bits)
	
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + SIZE_8 + SIZE_64 + SIZE_32 + *length;
	
	char* command = (char*)malloc(cmd_length);
	off_t pos = BuildCommand(command, VMW_CMD_WRITE_FILE, handle);
	SET_8(command, pos, 0);
	SET_64(command, pos, offset);
	SET_32(command, pos, *length);
	memcpy(command + pos, buffer, *length);
	pos += *length;

	ASSERT(pos == cmd_length);

	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);

	if (ret != B_OK)
		return ret;
	
	size_t msg_length;
	char* received = backdoor.GetMessage(&msg_length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (msg_length != 14) {
		free(received);
		return B_ERROR;
	}
	
	ret = ConvertStatus(*(uint32*)(received + 6));
	*length = *(uint32*)(received + 10);
	
	free(received);
	
	return B_OK;
}

status_t
VMWSharedFolders::CloseFile(file_handle handle)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Handle (32-bits, in BuildCommand)
	
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32;
	
	char* command = (char*)malloc(cmd_length);
	
	size_t pos = BuildCommand(command, VMW_CMD_CLOSE_FILE, handle);
	
	ASSERT(pos == cmd_length);
	
	status_t ret = backdoor.SendMessage(command, true, cmd_length);
	free(command);
	
	return ret;
}

status_t
VMWSharedFolders::OpenDir(const char* path, folder_handle* handle)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Path length (32-bits, in BuildCommand)
	// 3) The path itself (with / path delimiters replaced by null characters)
	
	const size_t path_length = strlen(path);
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + path_length + 1;
	
	char* command = (char*)malloc(cmd_length);
	
	off_t pos = BuildCommand(command, VMW_CMD_OPEN_DIR, path_length);
	CopyPath(path, command + pos, &pos);
	
	ASSERT(pos == cmd_length);
	
	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);
	
	if (ret != B_OK)
		return ret;
	
	size_t length;
	char* received = backdoor.GetMessage(&length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (length != 14) {
		free(received);
		return B_ERROR;
	}

	ret = ConvertStatus(*(uint32*)(received + 6));
	*handle = *(uint32*)(received + 10);
	
	free(received);
	
	return ret;
}

status_t
VMWSharedFolders::ReadDir(folder_handle handle, uint32 index, char* name, size_t max_length)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Handle (32-bits, in BuildCommand)
	// 3) Max name length (32-bits)
	
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + SIZE_32;
	
	char* command = (char*)malloc(cmd_length);
	off_t pos = BuildCommand(command, VMW_CMD_READ_DIR, handle);
	SET_32(command, pos, index);

	ASSERT(pos == cmd_length);
		
	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);
	
	if (ret != B_OK)
		return ret;
	
	size_t length;
	char* received = backdoor.GetMessage(&length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (length < 59) {
		free(received);
		return B_ERROR;
	}
	
	size_t name_length = *(uint32*)(received + 55);
	if (name_length == 0) {
		free(received);
		return B_ENTRY_NOT_FOUND;
	}
	
	if (name_length > max_length - 1) {
		free(received);
		return B_BUFFER_OVERFLOW;
	}
	
	strncpy(name, received + 59, max_length - 1);
	name[name_length] = '\0';
	
	free(received);
	
	return B_OK;
}

status_t
VMWSharedFolders::CloseDir(folder_handle handle)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Handle (32-bits, in BuildCommand)
	
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32;
	
	char* command = (char*)malloc(cmd_length);
	
	size_t pos = BuildCommand(command, VMW_CMD_CLOSE_DIR, handle);
	
	ASSERT(pos == cmd_length);

	status_t ret = backdoor.SendMessage(command, true, 14);
	free(command);
	return ret;
}

status_t
VMWSharedFolders::GetAttributes(const char* path, vmw_attributes* attributes, bool* is_dir)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Path length (32-bits, in BuildCommand)
	// 3) The path itself (with / path delimiters replaced by null characters)
	
	const size_t path_length = strlen(path);
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + path_length + 1;
	
	char* command = (char*)malloc(cmd_length);
	
	off_t pos = BuildCommand(command, VMW_CMD_GET_ATTR, path_length);
	CopyPath(path, command + pos, &pos);
	
	ASSERT(pos == cmd_length);
	
	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);
	
	if (ret != B_OK)
		return ret;
	
	size_t length;
	char* received = backdoor.GetMessage(&length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (length == 10) {
		free(received);
		return B_ENTRY_NOT_FOUND;
	}	
	
	if (length != 55) {
		free(received);
		return B_ERROR;
	}

	ret = ConvertStatus(*(uint32*)(received + 6));
	
	if (is_dir != NULL)
		*is_dir = (*(uint32*)(received + 10) == 0 ? false : true);
	
	if (attributes != NULL)
		memcpy(attributes, received + 14, sizeof(vmw_attributes));
	
	free(received);
	
	return ret;
}

status_t
VMWSharedFolders::SetAttributes(const char* path, const vmw_attributes* attributes, uint32 mask)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Attribute mask (32-bits, in BuildCommand)
	// 3) ???? (8-bits)
	// 4) attributes
	// 5) Path length (32-bits)
	// 6) The path itself (with / path delimiters replaced by null characters)
	
	const size_t path_length = strlen(path);
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + SIZE_8 + sizeof(vmw_attributes) + SIZE_32 + path_length + 1;
	
	char* command = (char*)malloc(cmd_length);
	
	off_t pos = BuildCommand(command, VMW_CMD_SET_ATTR, mask);
	SET_8(command, pos, 0);
	memcpy(command + pos, attributes, sizeof(vmw_attributes));
	pos += sizeof(vmw_attributes);
	SET_32(command, pos, path_length);	
	CopyPath(path, command + pos, &pos);
	
	ASSERT(pos == cmd_length);
	
	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);
	
	if (ret != B_OK)
		return ret;
	
	size_t length;
	char* received = backdoor.GetMessage(&length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (length != 10) {
		dprintf("SetAttributes: incorrect length: %ld\n", length);
		free(received);
		return B_ERROR;
	}

	ret = ConvertStatus(*(uint32*)(received + 6));
	
	free(received);
	
	return ret;
}

status_t
VMWSharedFolders::CreateDir(const char* path, uint8 mode)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) mode (8-bits, in BuildCommand)
	// 3) Path length (32-bits)
	// 4) The path itself (with / path delimiters replaced by null characters)
	
	const size_t path_length = strlen(path);
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_8 + SIZE_32 + path_length + 1;
	
	char* command = (char*)malloc(cmd_length);
	
	off_t pos = BuildCommand(command, VMW_CMD_NEW_DIR, 0);
	pos -= SIZE_32;
	SET_8(command, pos, mode);
	SET_32(command, pos, path_length);
	CopyPath(path, command + pos, &pos);
	
	ASSERT(pos == cmd_length);
	
	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);
	
	if (ret != B_OK)
		return ret;
	
	size_t length;
	char* received = backdoor.GetMessage(&length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (length != 10) {
		dprintf("CreateDir: incorrect length: %ld\n", length);
		free(received);
		return B_ERROR;
	}

	ret = ConvertStatus(*(uint32*)(received + 6));
	
	free(received);
	
	return ret;
}

status_t
VMWSharedFolders::Delete(const char* path, bool is_dir)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Path length (32-bits, in BuildCommand)
	// 3) The path itself (with / path delimiters replaced by null characters)
	
	const size_t path_length = strlen(path);
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + path_length + 1;
	
	char* command = (char*)malloc(cmd_length);
	
	off_t pos = BuildCommand(command, (is_dir ? VMW_CMD_DEL_DIR : VMW_CMD_DEL_FILE), path_length);
	CopyPath(path, command + pos, &pos);
	
	ASSERT(pos == cmd_length);
	
	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);
	
	if (ret != B_OK)
		return ret;
	
	size_t length;
	char* received = backdoor.GetMessage(&length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (length != 10) {
		dprintf("Delete: incorrect length: %ld\n", length);
		free(received);
		return B_ERROR;
	}

	ret = ConvertStatus(*(uint32*)(received + 6));
	
	free(received);
	
	return ret;
}

status_t
VMWSharedFolders::DeleteFile(const char* path)
{
	CALLED();
	return Delete(path, false);
}

status_t
VMWSharedFolders::DeleteDir(const char* path)
{
	CALLED();
	return Delete(path, true);
}

status_t
VMWSharedFolders::Move(const char* path_orig, const char* path_dest)
{
	CALLED();
	// Command string :
	// 0) Magic value (6 bytes, in BuildCommand)
	// 1) Command number (32-bits, in BuildCommand)
	// 2) Original path length (32-bits)
	// 3) The path itself (with / path delimiters replaced by null characters)
	// 4) Destination path length (32-bits)
	// 5) The path itself (with / path delimiters replaced by null characters)
	
	
	const size_t path_orig_length = strlen(path_orig);
	const size_t path_dest_length = strlen(path_dest);
	const size_t cmd_length = SIZE_START + SIZE_32 + SIZE_32 + path_orig_length + 1 \
		+ SIZE_32 + path_dest_length + 1;
	
	char* command = (char*)malloc(cmd_length);
	
	dprintf("Move: moving %s to %s...\n", path_orig, path_dest);
	
	off_t pos = BuildCommand(command, VMW_CMD_MOVE_FILE, path_orig_length);
	CopyPath(path_orig, command + pos, &pos);
	SET_32(command, pos, path_dest_length);	
	CopyPath(path_dest, command + pos, &pos);
	
	ASSERT(pos == cmd_length);
	
	status_t ret = backdoor.SendMessage(command, false, cmd_length);
	free(command);
	
	if (ret != B_OK)
		return ret;
	
	size_t length;
	char* received = backdoor.GetMessage(&length);
	
	if (received == NULL)
		return B_ERROR;
	
	if (length != 10) {
		dprintf("Move: incorrect length: %ld\n", length);
		free(received);
		return B_ERROR;
	}

	ret = ConvertStatus(*(uint32*)(received + 6));
	
	free(received);
	
	return ret;
}

off_t
VMWSharedFolders::BuildCommand(char* cmd_buffer, uint32 command, uint32 param)
{
	CALLED();
	const char start_bytes[] = { 'f', ' ', '\0', '\0', '\0', '\0' };
	
	memcpy(cmd_buffer, start_bytes, sizeof(start_bytes));
	
	off_t pos = sizeof(start_bytes);
	
	SET_32(cmd_buffer, pos, command);
	SET_32(cmd_buffer, pos, param);

	return pos;
}

status_t
VMWSharedFolders::ConvertStatus(int vmw_status)
{
	CALLED();
	switch (vmw_status) {
		case 0:		return B_OK;
		case 1:		return B_ENTRY_NOT_FOUND;
		case 3:		return B_PERMISSION_DENIED;
		case 4:		return B_FILE_EXISTS;
		case 6:		return B_DIRECTORY_NOT_EMPTY;
		case 8:		return B_NAME_IN_USE;
		default:	return B_ERROR;
	}
}

void
VMWSharedFolders::CopyPath(const char* path, char* dest, off_t* pos)
{	
	while (*path != '\0') {
		*dest = (*path == '/' ? '\0' : *path);
		dest++;
		path++;
		(*pos)++;
	}
	*dest = '\0';
	(*pos)++;
}