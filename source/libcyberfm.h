/*
Public domain.

David Reid - mackron@gmail.com
*/
#ifndef libcyberfm_h
#define libcyberfm_h

#include "external/minifs/minifs.h"
#include <stdint.h>

typedef void* cyberfm_handle;
typedef void (* cyberfm_proc)(void);

typedef unsigned int cyberfm_bool32;
#define CYBERFM_FALSE   0
#define CYBERFM_TRUE    1

typedef int cyberfm_result;
#define CYBERFM_SUCCESS              0
#define CYBERFM_ERROR               -1
#define CYBERFM_INVALID_ARGS        -2
#define CYBERFM_INVALID_OPERATION   -3
#define CYBERFM_OUT_OF_MEMORY       -4
#define CYBERFM_OUT_OF_RANGE        -5
#define CYBERFM_ACCESS_DENIED       -6
#define CYBERFM_DOES_NOT_EXIST      -7

#define CYBERFM_AUDIO_FORMAT_PCM    0x3102
#define CYBERFM_AUDIO_FORMAT_OPUS   0x4101

typedef struct cyberfm_archive cyberfm_archive;
typedef struct cyberfm_file    cyberfm_file;

/*
Cyperpunk 2077 uses Oodle for compression. Unfortunately we don't have public access to the official Oodle
headers, but we can write our own version of the necessary function declarations and dynamically load the
APIs from a DLL from another game distribution. This program does *not* include any Oodle stuff - DLLs
need to be sourced from elsewhere.

These function signatures were taken from the ZenHAX forum:
    https://zenhax.com/viewtopic.php?t=7292

Also thanks to this forum post regarding compression signatures for the tip about Oodle:
    https://zenhax.com/viewtopic.php?t=27
*/
typedef int (* cyberfm_OodleLZ_Decompress_proc)(unsigned char* pCompressedData, int compressedSize, unsigned char* pDecompressedData, int decompressedSize, int a, int b, int c, void* d, void* e, void* f, void* g, void* h, void* i, int j);


/*
This is a bit weird. I know that the first item must be the hashed version of the file name. Also, it looks like maybe "file"
is made up of a range of sub-files or something? The 4th and 5th members seem to be an range that correlates with the the
capacity of the list of data specs (the list containing the offsets and sizes of files). I was wondering if maybe this was
intended for things like 3D models, where you have one file for the main model definition, but then other files for materials
that are associated with the model. I'm not entirely sure, but certainly this caused a bit of confusion for me when trying
to figure this one out.
*/
typedef struct
{
    uint64_t hashedName;            /* This must be the hashed name as it's in ascending order, which would make sense as it would speed up lookups (would allow binary searching). */
    uint64_t unknown1;              /* This looks suspiciously like a time value. Last modified time? */
    uint32_t unknown2;              /* I'm not sure what this is for. I'm suspecting it's some kind of file type identifier? Does not seem to be a compression type identifier as everything seems to use Oodle. */
    uint32_t dataSpecRangeBeg;      /* The beginning (inclusive) index into the data spec list containing the offset and size of the file. */
    uint32_t dataSpecRangeEnd;      /* The ending (non inclusive) index into the data spec list containing the offset and size of the file. */
    uint32_t unknownDataRangeBeg;   /* This is similar to `dataSpecRangeBeg` and `dataSpecRangeEnd`, only it's a lookup into the as-yet-unkown section in the central directory. */
    uint32_t unknownDataRangeEnd;   /* ^^ As Above ^^ */
    uint32_t hash[5];               /* This looks suspiciously like an SHA-1 hash. 20 bytes of seemingly random data. Will test to confirm. */
} cyberfm_archive_file_info;

typedef struct
{
    uint64_t offset;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
} cyberfm_archive_file_data_spec;

/*
I have not been able to figure out what the third section in the central directory is intended for... But it's 8 bytes
in total for each item contained within it.

For the life of me I've not been able to figure out what this could possibly be used for...
*/
typedef struct
{
    uint64_t unknown0;
    uint64_t unknown1;
} cyberfm_archive_central_directory_unknown_data;

typedef struct
{
    uint32_t fourcc;    /* I'm not entirely sure what this is, but it seems to always be set to 0x08000000. */
    uint32_t size;      /* The combined size of each section in the central directory, not including the first 8 bytes (this variable and the FourCC). */
    uint64_t unknown0;  /* Some kind of hash/CRC? */
    uint32_t fileInfoCount;
    uint32_t fileDataSpecCount;
    uint32_t unknownDataCount;
    cyberfm_archive_file_info* pFileInfo;
    cyberfm_archive_file_data_spec* pFileDataSpec;
    cyberfm_archive_central_directory_unknown_data* pUnknownData;
    char pPayload[1]; /* The raw data of the central directory as a single allocation. Pointers above are offsets into this. */
} cyberfm_archive_central_directory;

struct cyberfm_archive
{
    FILE* pFile;
    uint32_t fourcc;
    uint32_t unknown0;      /* Always set to 0x0C000000. I think this is a fourcc that's intended to describe some kind of chunk of data in the archive. */
    uint64_t centralDirOffset;
    uint64_t centralDirSize;
    uint64_t unknown1;
    uint64_t archiveSize;   /* The size of the archive file. */
    uint8_t unknown2[132];  /* Padding? */
    cyberfm_archive_central_directory* pCentralDirectory;   /* Must be dynamically allocated. */
    struct
    {
        cyberfm_handle hOodle;  /* A handle to the Oodle shared object for loading OodleLZ_Decompress() */
        cyberfm_OodleLZ_Decompress_proc OodleLZ_Decompress;
    } oodle;
};

struct cyberfm_file
{
    cyberfm_archive* pArchive;
    uint64_t cursor;
    uint64_t size;
    uint8_t pData[1];   /* Lazy implementation for now - I'm just allocating all of the memory for the file on the heap. This won't scale. This is where memory mapping the archive would come in handy. */
};

cyberfm_result cyberfm_archive_init(const char* pFilePath, cyberfm_archive* pArchive);
void cyberfm_archive_uninit(cyberfm_archive* pArchive);

/*
Opens a file in the archive. I'm not sure yet how the whole sub-file thing is supposed to work, so for now
you need to specify an index. In the future it would be good to figure out the hashing algorithm used so
we can supporting opening files by their name.
*/
cyberfm_result cyberfm_file_open_by_index(cyberfm_archive* pArchive, uint32_t index, uint32_t subfile, cyberfm_file** ppFile);
cyberfm_result cyberfm_file_open(cyberfm_archive* pArchive, uint64_t hashedName, uint32_t subfile, cyberfm_file** ppFile);
void cyberfm_file_close(cyberfm_file* pFile);
cyberfm_result cyberfm_file_read(cyberfm_file* pFile, void* pData, size_t dataSize);
cyberfm_result cyberfm_file_seek(cyberfm_file* pFile, int64_t offset, int origin);
cyberfm_bool32 cyberfm_file_eof(cyberfm_file* pFile);



/*
Audio
=====
Audio files seem to be wrapped in a RIFF container, but it's not an actual WAV file. I've seen Opus encoded data as well as non-Opus data. I have
not yet figured out the non-Opus format, but I suspect it's uncompressed PCM data.

The problem is that although it's in a RIFF file, it doesn't actually conform to the WAV spec which means libraries like dr_wav cannot be used to
load the file because a good library won't actually allow those files to pass validation. If it was a valid WAV file, we could use dr_wav to open
the file, and then extract the "data" chunk, which is where the Opus encoded data is located, and then use libopus to decode that data.

What we're doing here is extracting the audio data from the invalid WAV format into a format that is valid. For audio files that use Opus encoding
we're going to extract the Opus audio. For other formats we'll create a valid and playable WAV file.
*/
cyberfm_result cyberfm_file_extract_audio(cyberfm_file* pFile, void** ppData, size_t* pDataSize, int* pDataFormat);


#endif  /* libcyberfm */
