# kitty-graphics-transmission Specification

## Purpose
Lets programs send images to paw with the kitty graphics protocol, store them under ids, query support, and get replies, within a fixed memory quota.

## Requirements

### Requirement: Graphics command recognition
paw SHALL treat an APC string that starts with `G` (`ESC _ G <control data> [; <payload>] ESC \`) as a graphics command. The control data is a comma-separated list of `key=value` pairs. The payload is base64. paw SHALL continue to ignore other APC strings, and SOS and PM strings.

#### Scenario: Graphics command is handled
- **WHEN** a program writes `ESC _ G a=q,i=1,s=1,v=1,f=24 ; AAAA ESC \`
- **THEN** paw handles it as a query and replies

#### Scenario: Other APC string is ignored
- **WHEN** a program writes `ESC _ hello ESC \`
- **THEN** paw shows nothing, sends no reply, and continues to parse the input after the string

#### Scenario: Malformed control data
- **WHEN** a graphics command has a value that is not a number where the key needs a number
- **THEN** paw rejects the command with an `EINVAL` error reply (when a reply is due) and changes no state

#### Scenario: Unknown key
- **WHEN** a graphics command has a key that paw does not know (for example the usage hint `N=1`)
- **THEN** paw ignores that key and handles the rest of the command

### Requirement: Pixel formats
paw SHALL accept image data as 24-bit RGB (`f=24`), 32-bit RGBA (`f=32`, the default), and PNG (`f=100`). For `f=24` and `f=32`, the width `s` and height `v` are required and the data length SHALL be exactly `s * v * 3` or `s * v * 4` bytes. For PNG, paw SHALL read the size from the PNG data.

#### Scenario: RGB image
- **WHEN** a program transmits `f=24,s=10,v=20` with 600 bytes of pixel data
- **THEN** paw stores a 10x20 image

#### Scenario: Wrong data length
- **WHEN** a program transmits `f=32,s=10,v=10` with 300 bytes of pixel data
- **THEN** paw stores no image and replies with an error (when a reply is due)

#### Scenario: Invalid PNG
- **WHEN** a program transmits `f=100` with data that is not a valid PNG
- **THEN** paw stores no image, replies with an error (when a reply is due), and continues to run

### Requirement: Compression
When the command has `o=z`, paw SHALL inflate the data with RFC 1950 zlib before it reads the pixel format. Any other value of `o` SHALL be an error.

#### Scenario: Compressed RGBA
- **WHEN** a program transmits `f=32,s=4,v=4,o=z` with zlib data that inflates to 64 bytes
- **THEN** paw stores a 4x4 image

#### Scenario: Corrupt compressed data
- **WHEN** the zlib data cannot be inflated
- **THEN** paw stores no image and replies with an error (when a reply is due)

### Requirement: Chunked direct transfer
With direct transmission (`t=d`, the default), paw SHALL join the payloads of commands with `m=1` and the next command with `m=0` into one image. Only the first chunk carries the full control data. A delete command received during a chunked transfer SHALL cancel the transfer.

#### Scenario: Three chunks
- **WHEN** a program sends a transmit command with `m=1`, then a command with only `m=1` and data, then a command with only `m=0` and data
- **THEN** paw stores one image built from the three payloads

#### Scenario: Transfer cancelled by delete
- **WHEN** a chunked transfer is in progress and a program sends `a=d`
- **THEN** paw discards the partial data and stores no image from it

### Requirement: Image ids and numbers
An image transmitted with `i=<id>` SHALL replace any image with the same id and delete all placements of the old image. An image transmitted with `I=<number>` SHALL get a new id chosen by paw, and later commands that use `I=<number>` SHALL refer to the newest image with that number. A command with both `i` and `I` SHALL fail with `EINVAL`.

#### Scenario: Re-transmit an id
- **WHEN** image 5 has a placement on screen and a program transmits new data with `i=5`
- **THEN** the old placement disappears and image 5 holds the new data

#### Scenario: Image number
- **WHEN** a program transmits with `I=13`
- **THEN** paw replies `ESC _ G i=<new id>,I=13 ; OK ESC \`

#### Scenario: Both i and I
- **WHEN** a command has `i=1,I=2`
- **THEN** paw replies with an error that starts with `EINVAL`

### Requirement: Replies
paw SHALL reply to a command that has `i` or `I` with `ESC _ G i=<id>[,I=<number>][,p=<placement id>] ; <message> ESC \`. The message is `OK` on success. On failure it is an error code and text (for example `ENOENT:...`), with printable ASCII only. With `q=1` paw SHALL NOT send `OK` replies. With `q=2` paw SHALL NOT send any replies. Without `i` and `I`, paw SHALL send no reply.

#### Scenario: Successful transmit with id
- **WHEN** a program transmits a valid image with `i=7`
- **THEN** paw replies `ESC _ G i=7 ; OK ESC \`

#### Scenario: Quiet success
- **WHEN** a program transmits a valid image with `i=7,q=1`
- **THEN** paw sends no reply

#### Scenario: Quiet failure
- **WHEN** a program places a missing image with `a=p,i=99,q=2`
- **THEN** paw sends no reply

#### Scenario: Place a missing image
- **WHEN** a program sends `a=p,i=99` and image 99 does not exist
- **THEN** paw replies with a message that starts with `ENOENT`

### Requirement: Query action
For `a=q`, paw SHALL load and check the image data as for a transmit and reply with `OK` or an error. paw SHALL NOT store the image and SHALL NOT replace an image with the same id. paw SHALL send the reply before it handles any later input bytes.

#### Scenario: Support detection
- **WHEN** a program writes `ESC _ G i=31,s=1,v=1,a=q,t=d,f=24 ; AAAA ESC \` followed by `ESC [ c`
- **THEN** paw sends `ESC _ G i=31 ; OK ESC \` before the primary device attributes reply

#### Scenario: Query keeps existing image
- **WHEN** image 31 exists and a program sends a query with `i=31`
- **THEN** image 31 is not changed

### Requirement: Storage quota
paw SHALL keep image data for each screen buffer (main and alternate) separately, with a limit of 320 MB of decoded pixel data per buffer. When a new image would exceed the limit, paw SHALL delete images without placements first, oldest first, then the oldest images with placements, until the new image fits. An image larger than the limit SHALL fail.

#### Scenario: Quota eviction
- **WHEN** the stored images use 300 MB and a program transmits a 40 MB image
- **THEN** paw deletes the oldest images until the new image fits, and stores the new image

#### Scenario: Image larger than the quota
- **WHEN** a program transmits an image that decodes to more than 320 MB
- **THEN** paw stores no image, deletes no other image, and replies with an error (when a reply is due)

### Requirement: Unsupported actions
paw SHALL reject the animation actions (`a=f`, `a=a`, `a=c`) and relative placements (`P` or `Q` keys) with an error reply (when a reply is due), and SHALL change no state.

#### Scenario: Animation frame command
- **WHEN** a program sends `a=f,i=1` with frame data
- **THEN** paw replies with an error and image 1 does not change
