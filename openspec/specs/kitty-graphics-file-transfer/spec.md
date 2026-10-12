# kitty-graphics-file-transfer Specification

## Purpose
Lets local programs give paw image data through a file, a temp file, or POSIX shared memory, without letting a remote or sandboxed program read, probe, or delete files it could not access by itself.

## Requirements

### Requirement: File transmission
With `t=f`, the payload SHALL be a base64 file path. paw SHALL resolve symlinks, then read the file. The `O` key gives a byte offset and the `S` key gives a byte count. When `S` is 0, paw SHALL read to the end of the file. paw SHALL NOT delete the file.

#### Scenario: Read a PNG file
- **WHEN** a program sends `a=T,t=f,f=100` with the base64 path of a valid PNG file
- **THEN** paw shows the image and the file still exists

#### Scenario: Offset and size
- **WHEN** a program sends `t=f,f=24,s=2,v=2,O=10,S=12` for a file of 40 bytes
- **THEN** paw reads bytes 10 to 21 of the file as the pixel data

### Requirement: Only regular files outside sensitive paths
For `t=f` and `t=t`, paw SHALL read only regular files. After it resolves symlinks, paw SHALL refuse paths under `/proc`, `/sys`, and `/dev`, except paths under `/dev/shm`. paw SHALL check the path before it opens the file, open it without blocking, and check again that the open file is a regular file.

#### Scenario: Device file
- **WHEN** a program sends `t=f` with the path `/dev/zero`
- **THEN** paw does not read the file and replies with the file error

#### Scenario: Symlink to a sensitive path
- **WHEN** a program sends `t=f` with the path of a symlink that points to `/proc/self/environ`
- **THEN** paw does not read the file and replies with the file error

#### Scenario: Named pipe
- **WHEN** a program sends `t=f` with the path of a FIFO
- **THEN** paw does not block, does not read, and replies with the file error

#### Scenario: Symlink loop
- **WHEN** a program sends `t=f` with a path that has a symlink loop
- **THEN** paw replies with the file error

### Requirement: One error for all file failures
Every failure to read image data from a file or shared memory object SHALL produce the same reply: `EBADF:Failed to read image file`. This includes a missing file, no permission, a refused file type or path, and a file shorter than `O + S`. paw SHALL write the real reason only to its own log.

#### Scenario: Missing file and refused file look the same
- **WHEN** a program sends `t=f,i=1` for a path that does not exist, and then `t=f,i=2` for `/dev/zero`
- **THEN** both replies have the message `EBADF:Failed to read image file`

#### Scenario: File too short
- **WHEN** a program sends `t=f,i=1,O=0,S=100` for a 50-byte file
- **THEN** the reply message is `EBADF:Failed to read image file`

### Requirement: Temp file transmission
With `t=t`, paw SHALL read the file as for `t=f` and then delete it, but only when the resolved path is inside `/tmp`, `/dev/shm`, or `$TMPDIR` and contains the text `tty-graphics-protocol`. In all other cases paw SHALL read the file (when the read rules allow it) and SHALL NOT delete it.

#### Scenario: Valid temp file
- **WHEN** a program sends `t=t` for `/tmp/tty-graphics-protocol-abc.png`
- **THEN** paw shows the image and the file no longer exists

#### Scenario: Temp file without the marker text
- **WHEN** a program sends `t=t` for `/tmp/photo.png`
- **THEN** paw reads the file and the file still exists

#### Scenario: Marker text outside a temp directory
- **WHEN** a program sends `t=t` for `$HOME/tty-graphics-protocol.png`
- **THEN** the file still exists after the command

### Requirement: Shared memory transmission
With `t=s`, the payload SHALL be a base64 POSIX shared memory name. The name SHALL start with `/`, contain no other `/`, and be no longer than the system limit. paw SHALL read the data with the `O` and `S` keys as for files, then unlink and close the object. paw SHALL unlink the object also when the read or decode fails, but not when the name is invalid.

#### Scenario: Valid shared memory object
- **WHEN** a program writes RGBA data to shm object `/kitty-img-1` and sends `a=T,t=s,f=32,s=8,v=8`
- **THEN** paw shows the image and `/dev/shm/kitty-img-1` no longer exists

#### Scenario: Bad data in shared memory
- **WHEN** the shm object holds data that is too short for `s` and `v`
- **THEN** paw replies with the file error and the object is unlinked

#### Scenario: Invalid name
- **WHEN** a program sends `t=s` with the name `/a/b`
- **THEN** paw replies with the file error and unlinks nothing
