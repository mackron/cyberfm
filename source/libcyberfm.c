/*
Public domain.

David Reid - mackron@gmail.com
*/
#include "libcyberfm.h"

#define MINIFS_IMPLEMENTATION
#include "external/minifs/minifs.h"

#define DR_WAV_IMPLEMENTATION
#include "external/dr_libs/dr_wav.h"

#define CYBERFM_ZERO_OBJECT(p)          memset(p, 0, sizeof(*p))
#define CYBERFM_OFFSET_PTR(p, offset)   (((uint8_t*)(p)) + (offset))


/*
This is a list of Oodle shared objects to try loading, in order. If I've missed any here, feel free to let
me know and I'll add it to the list.
*/
static const char* g_cyberfmOodleSONames[] = {
    "oo2core_8_win64.dll"
};


cyberfm_handle cyberfm_dlopen(const char* filename)
{
    cyberfm_handle handle;

#ifdef _WIN32
    handle = (cyberfm_handle)LoadLibraryA(filename);
#else
    handle = (cyberfm_handle)dlopen(filename, RTLD_NOW);
#endif

    return handle;
}

void cyberfm_dlclose(cyberfm_handle handle)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose((void*)handle);
#endif
}

cyberfm_proc cyberfm_dlsym(cyberfm_handle handle, const char* symbol)
{
    cyberfm_proc proc;

#ifdef _WIN32
    proc = (cyberfm_proc)GetProcAddress((HMODULE)handle, symbol);
#else
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
#endif
    proc = (cyberfm_proc)dlsym((void*)handle, symbol);
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
    #pragma GCC diagnostic pop
#endif
#endif

    return proc;
}



static cyberfm_result cyberfm_result_from_minifs(cyberfm_result result)
{
    return (cyberfm_result)result;  /* Result codes should be the same. */
}

cyberfm_result cyberfm_archive_init(const char* pFilePath, cyberfm_archive* pArchive)
{
    cyberfm_result result;
    FILE* pFile;
    struct _stat64 info;
    uint64_t fileInfoChunkSize;
    uint64_t fileDataSpecChunkSize;
    uint64_t unknownDataChunkSize;
    size_t iOodleSOName;

    if (pArchive == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    CYBERFM_ZERO_OBJECT(pArchive);

    if (pFilePath == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    /*
    Try loading Oodle. It's not a critical error if this is not avaiable, but compressed files won't be able
    to be opened.
    */
    for (iOodleSOName = 0; iOodleSOName < sizeof(g_cyberfmOodleSONames)/sizeof(g_cyberfmOodleSONames[0]); iOodleSOName += 1) {
        pArchive->oodle.hOodle = cyberfm_dlopen(g_cyberfmOodleSONames[iOodleSOName]);
        if (pArchive->oodle.hOodle != NULL) {
            pArchive->oodle.OodleLZ_Decompress = (cyberfm_OodleLZ_Decompress_proc)cyberfm_dlsym(pArchive->oodle.hOodle, "OodleLZ_Decompress");
            if (pArchive->oodle.OodleLZ_Decompress != NULL) {
                /* Everything looks good. */
                break;
            } else {
                /* The shared object is avaialable, but not the function. Keep trying. */
                cyberfm_dlclose(pArchive->oodle.hOodle);
                pArchive->oodle.hOodle = NULL;
            }
        }
    }
    

    result = cyberfm_result_from_minifs(mfs_fopen(&pFile, pFilePath, "rb"));
    if (result != CYBERFM_SUCCESS) {
        goto error0;
    }

    /* We opened the file successfully. We can now start extracting some data and do some validation checks. */
    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->fourcc, sizeof(pArchive->fourcc), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error1;
    }

    if (pArchive->fourcc != 0x52414452) {
        result = CYBERFM_ERROR; /* Not a valid archive file. */
        goto error1;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->unknown0, sizeof(pArchive->unknown0), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error1;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->centralDirOffset, sizeof(pArchive->centralDirOffset), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error1;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->centralDirSize, sizeof(pArchive->centralDirSize), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error1;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->unknown1, sizeof(pArchive->unknown1), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error1;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->archiveSize, sizeof(pArchive->archiveSize), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error1;
    }

    /* Not going to bother reading the padding. */

    /* Quick validation check of the central directory offset + size. */
    if ((pArchive->centralDirOffset + pArchive->centralDirSize) > pArchive->archiveSize) {
        goto error1;    /* Central directory is invalid. */
    }

    /* Quickly check that the file size is correct. */
    result = cyberfm_result_from_minifs(mfs_fstat(pFile, &info));
    if (result != CYBERFM_SUCCESS) {
        goto error1;
    }

    if ((pArchive->centralDirOffset + pArchive->centralDirSize) > (uint64_t)info.st_size) {
        goto error1;    /* Central directory is invalid. */
    }


    /*
    We now need to load the central directory. The amount of memory can be based on the size of the central directory
    we extracted earlier. Technically this will end up being more than we need, but it doesn't matter.
    */
    pArchive->pCentralDirectory = (cyberfm_archive_central_directory*)malloc(sizeof(*pArchive->pCentralDirectory) + pArchive->centralDirSize);
    if (pArchive->pCentralDirectory == NULL) {
        result = CYBERFM_OUT_OF_MEMORY;
        goto error1;
    }

    result = cyberfm_result_from_minifs(mfs_fseek(pFile, (mfs_int64)pArchive->centralDirOffset, SEEK_SET));
    if (result != CYBERFM_SUCCESS) {
        goto error2;    /* Failed to seek to the central directory. */
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->pCentralDirectory->fourcc, sizeof(pArchive->pCentralDirectory->fourcc), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->pCentralDirectory->size, sizeof(pArchive->pCentralDirectory->size), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->pCentralDirectory->unknown0, sizeof(pArchive->pCentralDirectory->unknown0), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->pCentralDirectory->fileInfoCount, sizeof(pArchive->pCentralDirectory->fileInfoCount), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->pCentralDirectory->fileDataSpecCount, sizeof(pArchive->pCentralDirectory->fileDataSpecCount), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }

    result = cyberfm_result_from_minifs(mfs_fread(pFile, &pArchive->pCentralDirectory->unknownDataCount, sizeof(pArchive->pCentralDirectory->unknownDataCount), NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }


    /*
    We don't *technically* need to load this data into memory because we could just extract it from the file
    dynamically, but it makes it easier to use and memory usage shouldn't be too crazy. We could also map the
    file, but I'm not going to bother right now (might leave this as an excersize for later).

    Right now we're sitting on the first byte of the file info section. Each segment here is 56 bytes. We can
    therefore determine how much data to read and then do it all in a single read.
    */
    fileInfoChunkSize = pArchive->pCentralDirectory->fileInfoCount * 56;
    pArchive->pCentralDirectory->pFileInfo = (cyberfm_archive_file_info*)CYBERFM_OFFSET_PTR(pArchive->pCentralDirectory->pPayload, 0);

    result = cyberfm_result_from_minifs(mfs_fread(pFile, pArchive->pCentralDirectory->pFileInfo, fileInfoChunkSize, NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }

    /* The data specs are 16 bytes each. */
    fileDataSpecChunkSize = pArchive->pCentralDirectory->fileDataSpecCount * 16;
    pArchive->pCentralDirectory->pFileDataSpec = (cyberfm_archive_file_data_spec*)CYBERFM_OFFSET_PTR(pArchive->pCentralDirectory->pPayload, fileInfoChunkSize);

    result = cyberfm_result_from_minifs(mfs_fread(pFile, pArchive->pCentralDirectory->pFileDataSpec, fileDataSpecChunkSize, NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }

    /* The as yet unknown section is 8 bytes each. */
    unknownDataChunkSize = pArchive->pCentralDirectory->unknownDataCount * 8;
    pArchive->pCentralDirectory->pUnknownData = (cyberfm_archive_central_directory_unknown_data*)CYBERFM_OFFSET_PTR(pArchive->pCentralDirectory->pPayload, fileInfoChunkSize + fileDataSpecChunkSize);

    result = cyberfm_result_from_minifs(mfs_fread(pFile, pArchive->pCentralDirectory->pUnknownData, unknownDataChunkSize, NULL));
    if (result != CYBERFM_SUCCESS) {
        goto error2;
    }


    /* We're done. The file needs to be left open so we can extract data later. */
    pArchive->pFile = pFile;

    return CYBERFM_SUCCESS;

error2: free(pArchive->pCentralDirectory);
error1: mfs_fclose(pFile);
error0: return result;
}

void cyberfm_archive_uninit(cyberfm_archive* pArchive)
{
    if (pArchive == NULL) {
        return;
    }

    free(pArchive->pCentralDirectory);
    mfs_fclose(pArchive->pFile);
}

cyberfm_result cyberfm_archive_find(cyberfm_archive* pArchive, uint64_t hashedName, uint32_t* pFileIndex)
{
    uint32_t iFile;

    if (pArchive == NULL || pFileIndex == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    /* TODO: Binary search. */
    for (iFile = 0; iFile < pArchive->pCentralDirectory->fileInfoCount; iFile += 1) {
        if (pArchive->pCentralDirectory->pFileInfo[iFile].hashedName == hashedName) {
            *pFileIndex = iFile;
            return CYBERFM_SUCCESS;
        }
    }

    /* Getting here means the file could not be found. */
    return CYBERFM_ERROR;
}


cyberfm_result cyberfm_file_open_by_index(cyberfm_archive* pArchive, uint32_t index, uint32_t subfile, cyberfm_file** ppFile)
{
    cyberfm_result result;
    uint32_t iDataSpec;
    cyberfm_file* pFile;

    if (ppFile == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    /* The sub-file needs to be within range. */
    if (subfile >= (pArchive->pCentralDirectory->pFileInfo[index].dataSpecRangeEnd - pArchive->pCentralDirectory->pFileInfo[index].dataSpecRangeBeg)) {
        return CYBERFM_INVALID_ARGS;    /* The sub-file is invalid. */
    }

    /*
    It looks like the file is good at so far. Now we need to get the data. If we were memory mapping the archive file we could
    avoid memory allocations, but for now we'll go ahead and use a malloc(). I don't think this method will scale.
    */
    iDataSpec = pArchive->pCentralDirectory->pFileInfo[index].dataSpecRangeBeg + subfile;

    pFile = (cyberfm_file*)malloc(sizeof(*pFile) + pArchive->pCentralDirectory->pFileDataSpec[iDataSpec].uncompressedSize);
    if (pFile == NULL) {
        return CYBERFM_OUT_OF_MEMORY;
    }

    pFile->pArchive = pArchive;
    pFile->cursor   = 0;
    pFile->size     = pArchive->pCentralDirectory->pFileDataSpec[iDataSpec].uncompressedSize;

    /* TODO: This needs to be made thread safe. */
    result = cyberfm_result_from_minifs(mfs_fseek(pArchive->pFile, pArchive->pCentralDirectory->pFileDataSpec[iDataSpec].offset, SEEK_SET));
    if (result != CYBERFM_SUCCESS) {
        free(pFile);
        return result;
    }

    if (pArchive->pCentralDirectory->pFileDataSpec[iDataSpec].compressedSize == pArchive->pCentralDirectory->pFileDataSpec[iDataSpec].uncompressedSize) {
        /* Not compressed. */
        result = cyberfm_result_from_minifs(mfs_fread(pArchive->pFile, pFile->pData, (size_t)pFile->size, NULL));
        if (result != CYBERFM_SUCCESS) {
            free(pFile);
            return result;
        }
    } else {
        /* Compressed. */
        uint32_t compressedSize;
        void* pCompressedData;
        int decompressionResult;

        if (pArchive->oodle.hOodle == NULL) {
            free(pFile);
            return CYBERFM_INVALID_OPERATION;
        }

        compressedSize = pArchive->pCentralDirectory->pFileDataSpec[iDataSpec].compressedSize;
        pCompressedData = malloc(compressedSize);
        if (pCompressedData == NULL) {
            free(pFile);
            return CYBERFM_OUT_OF_MEMORY;
        }

        result = cyberfm_result_from_minifs(mfs_fread(pArchive->pFile, pCompressedData, compressedSize, NULL));
        if (result != CYBERFM_SUCCESS) {
            free(pFile);
            return result;
        }

        /* TODO: Validate the compressed data to check the FourCC and that the decompressed sizes are equal. */

        decompressionResult = pArchive->oodle.OodleLZ_Decompress(CYBERFM_OFFSET_PTR(pCompressedData, 8), compressedSize, pFile->pData, pArchive->pCentralDirectory->pFileDataSpec[iDataSpec].uncompressedSize, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, 0);
        free(pCompressedData);

        if (decompressionResult != pArchive->pCentralDirectory->pFileDataSpec[iDataSpec].uncompressedSize) {
            free(pFile);
            return CYBERFM_ERROR;   /* Failed to decompress. */
        }
    }

    /* We're done. */
    *ppFile = pFile;

    return CYBERFM_SUCCESS;
}

cyberfm_result cyberfm_file_open(cyberfm_archive* pArchive, uint64_t hashedName, uint32_t subfile, cyberfm_file** ppFile)
{
    cyberfm_result result;
    uint32_t iFile;

    if (ppFile == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    *ppFile = NULL;

    if (pArchive == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    /* The first thing to do is find the file. */
    result = cyberfm_archive_find(pArchive, hashedName, &iFile);
    if (result != CYBERFM_SUCCESS) {
        return result;  /* The file was probably not found. */
    }

    return cyberfm_file_open_by_index(pArchive, iFile, subfile, ppFile);
}

void cyberfm_file_close(cyberfm_file* pFile)
{
    if (pFile == NULL) {
        return;
    }

    free(pFile);
}

cyberfm_result cyberfm_file_read(cyberfm_file* pFile, void* pData, size_t dataSize)
{
    if (pFile == NULL || pData == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    if (dataSize > (pFile->size - pFile->cursor)) {
        return CYBERFM_INVALID_ARGS;    /* Trying to read too much. */
    }

    memcpy(pData, pFile->pData + pFile->cursor, dataSize);
    pFile->cursor += dataSize;

    return CYBERFM_SUCCESS;
}

cyberfm_result cyberfm_file_seek(cyberfm_file* pFile, int64_t offset, int origin)
{
    if (pFile == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    switch (origin)
    {
        case SEEK_SET:
        {
            if (offset < 0 || (uint64_t)offset > pFile->size) {
                return CYBERFM_INVALID_ARGS;    /* Seeking out of bounds. */
            }

            pFile->cursor = (uint64_t)offset;
        } break;

        case SEEK_CUR:
        {
            if (offset < 0) {
                if ((uint64_t)-offset > pFile->cursor) {
                    return CYBERFM_INVALID_ARGS;    /* Trying to seek too far backwards. */
                }
            } else {
                if ((uint64_t)offset > (pFile->size - pFile->cursor)) {
                    return CYBERFM_INVALID_ARGS;    /* Trying to seek too far forward. */
                }
            }

            pFile->cursor = (uint64_t)(pFile->cursor + offset);
        } break;

        case SEEK_END:
        {
            if (offset < 0) {
                return CYBERFM_INVALID_ARGS;
            }

            pFile->cursor = pFile->size - offset;
        } break;

        default: return CYBERFM_INVALID_ARGS;
    }

    return CYBERFM_SUCCESS;
}

cyberfm_bool32 cyberfm_file_eof(cyberfm_file* pFile)
{
    if (pFile == NULL) {
        return CYBERFM_FALSE;
    }

    return pFile->cursor == pFile->size;
}


static cyberfm_bool32 cyberfm_does_data_look_like_opus(const void* pData, size_t dataSize)
{
    const char* pData8 = (const char*)pData;    /* To make it easier to inspect individual bytes. */

    /*
    The first part of the data should start with OggS (this is the Ogg encapsulation capture pattern) which
    all Ogg encapsulated streams should start with.
    */
    if (dataSize < 4) {
        return CYBERFM_FALSE;
    }

    if (pData8[0] != 'O' || pData8[1] != 'g' || pData8[2] != 'g' || pData8[3] != 'S') {
        return CYBERFM_FALSE;
    }

    /* TODO: Check this further. It's possible other forms could be Ogg encapsulated. When dr_opus is done, use that. */

    return CYBERFM_TRUE;
}

cyberfm_result cyberfm_file_extract_audio(cyberfm_file* pFile, void** ppData, size_t* pDataSize, int* pDataFormat)
{
    uint32_t fourcc;
    uint32_t chunkSize;
    uint16_t fmt_formatTag;
    uint16_t fmt_channels;
    uint32_t fmt_sampleRate;
    uint32_t fmt_avgBytesPerSec;
    uint16_t fmt_blockAlign;
    uint16_t fmt_bitsPerSample;
    uint16_t fmt_extendedSize;
    uint32_t fmt_customFormatCode;  /* This is the format code used in the Cyberpunk-specific data. */
    cyberfm_bool32 hasEncoderBeenInitializd = CYBERFM_FALSE;
    drwav wavEncoder;  /* Only used if we're using uncompressed audio. */
    void* pData = NULL; /* Important this is initialized to NULL. */
    size_t dataSize;

    if (pDataSize == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    *pDataSize = 0;

    if (pFile == NULL || ppData == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    /* First thing is to check that we're actually looking at an audio file. */
    if (pFile->pData[0] != 'R' || pFile->pData[1] != 'I' || pFile->pData[2] != 'F' || pFile->pData[3] != 'F') {
        return CYBERFM_ERROR;   /* Not an audio file. */
    }

    /* Note that from here out we're only supporting little-endian. */
    cyberfm_file_seek(pFile, 4, SEEK_SET);
    cyberfm_file_read(pFile, &chunkSize, 4);
    
    /* Next bytes should equal "WAVE". */
    cyberfm_file_read(pFile, &fourcc, 4);
    if (fourcc != 0x45564157) {
        return CYBERFM_ERROR;   /* Invalid audio file. */
    }

    /* Next should be the "fmt " chunk. */
    cyberfm_file_read(pFile, &fourcc, 4);
    if (fourcc != 0x20746D66) {
        return CYBERFM_ERROR;   /* Invalid audio file. */
    }

    cyberfm_file_read(pFile, &chunkSize, 4);

    /* We have a fmt chunk so now we can start reading it. */
    cyberfm_file_read(pFile, &fmt_formatTag,      2);
    cyberfm_file_read(pFile, &fmt_channels,       2);
    cyberfm_file_read(pFile, &fmt_sampleRate,     4);
    cyberfm_file_read(pFile, &fmt_avgBytesPerSec, 4);
    cyberfm_file_read(pFile, &fmt_blockAlign,     2);
    cyberfm_file_read(pFile, &fmt_bitsPerSample,  2);
    
    /*
    If we have some extended data we need to read it. This is where things get different in the Cyberpunk 2077 format
    to standard WAV. In standard WAV we expect the extended chunk size to be a very specific size when the format tag
    is set to 0xFFFE of 22. For some reason Cyberpunk's is only set to 6.
    */
    if (chunkSize > 16) {   /* <-- Pretty sure this will always be the case for Cyberpunk audio files... */
        cyberfm_file_read(pFile, &fmt_extendedSize, 2);

        if (fmt_formatTag == 0xFFFE || fmt_formatTag == 0xFFFF || fmt_formatTag == 0x3040) {
            if (fmt_extendedSize == 22) {
                /* Standard extended format. */
                return CYBERFM_ERROR;   /* TODO: Implement me. */
            } else {
                if (fmt_extendedSize >= 6) {
                    /*
                    This is the part that's specific to Cyberpunk 2077. I don't know exactly what the 6 bytes mean, but the
                    first 2 always seem to 0. Then it looks like the next 2 change between files. I've seen the following:

                        - 0x3102
                        - 0x4101

                    After than, the next two bytes seem to also always be zero. My instinct is that the last 4 bytes are
                    actually the format code. For now I'm running with this assumption.

                    Annoyingly, these two codes seem random. I've seen both be used for Opus encoded data, for example. I
                    think the best way to detect the contents is to inspect the actual data.
                    */
                    cyberfm_file_seek(pFile, 2, SEEK_CUR);  /* Seek past the unknown data. */
                    cyberfm_file_read(pFile, &fmt_customFormatCode, 4);

                    /*
                    I've seen some files with extended sizes like 48. Again, I think these contain some kind of Cyberpunk-
                    specific data. I'm not sure what this data contains so for now I'm just skipping over this.
                    */
                    if (fmt_extendedSize > 6) {
                        cyberfm_file_seek(pFile, fmt_extendedSize - 6, SEEK_CUR);
                    }
                } else {
                    return CYBERFM_ERROR;   /* Don't know how to handle this. */
                }
            }
        } else {
            /* Just seek past for non-extended formats. */
            cyberfm_file_seek(pFile, fmt_extendedSize, SEEK_CUR);
            fmt_customFormatCode = 0;
        }
    }

    /* Seek past any padding. */
    cyberfm_file_seek(pFile, (chunkSize % 2), SEEK_CUR);


    /*
    At this point we should have our "fmt " chunk handled. I think it's possible for multiple data chunks to be available
    which means we may want to output multiple files. I'm not sure how we should extract the data in this case...

    Note that the next chunk isn't necessarily the "data" chunk. I have seen "smpl", "cue " and "JUNK" chunks. It's the
    ones with "smpl" and "cue " that I'm unsure on as those files seem to contain multiple "data" chunks.
    */
    for (;;) {
        if (cyberfm_file_eof(pFile)) {
            break;
        }

        cyberfm_file_read(pFile, &fourcc,    4);
        cyberfm_file_read(pFile, &chunkSize, 4);

        /*
        For some reason the chunk size ends up being larger than the file which causes things to fall apart. I'm not sure why - I'm suspecting
        this may instead be getting used as a total frame count, maybe? In any case, it's not what it *should* be!
        */
        if (chunkSize > (uint32_t)(pFile->size - pFile->cursor)) {
            chunkSize = (uint32_t)(pFile->size - pFile->cursor);
        }

        if (fourcc == 0x61746164) { /* "data" */
            /*
            We found a "data" chunk. We need to inspect the data in order to know what it is. The custom
            format code specified in the "fmt " chunk is not reliable enough.

            I have seen a lot of Opus encoded files so I'm going to check if the data looks like Opus data,
            and if so just assume that.
            */
            const void* pChunkData = CYBERFM_OFFSET_PTR(pFile->pData, pFile->cursor);

            if (cyberfm_does_data_look_like_opus(pChunkData, chunkSize)) {
                /* Opus. For now just replace any previous data in the event of multiple "data" chunks. Might want to play around with some concatenation. */
                fmt_customFormatCode = CYBERFM_AUDIO_FORMAT_OPUS;

                if (pData != NULL) {
                    free(pData);
                }

                dataSize = chunkSize;
                pData = malloc(dataSize);
                if (pData == NULL) {
                    return CYBERFM_OUT_OF_MEMORY;
                }

                memcpy(pData, CYBERFM_OFFSET_PTR(pFile->pData, pFile->cursor), dataSize);
            } else {
                /*
                Not Opus. I'm not sure what the encoding for this one is. It doesn't seem to be Opus uncompressed, but I can't see any
                FourCCs or some other identifying markers. Maybe some kind of ADPCM? Maybe a custom compressed format? For now just
                outputting as an uncompressed stream. More investigation needed for this one.
                */
                fmt_customFormatCode = CYBERFM_AUDIO_FORMAT_PCM;

                if (!hasEncoderBeenInitializd) {
                    drwav_data_format format;
                    format.container     = drwav_container_riff;
                    format.format        = DR_WAVE_FORMAT_PCM;
                    format.channels      = fmt_channels;
                    format.sampleRate    = fmt_sampleRate;
                    format.bitsPerSample = fmt_bitsPerSample;
                    if (drwav_init_memory_write(&wavEncoder, &pData, &dataSize, &format, NULL) != DRWAV_TRUE) {
                        return CYBERFM_ERROR;   /* Failed to initialize WAV encoder. */
                    }

                    hasEncoderBeenInitializd = CYBERFM_TRUE;
                }

                drwav_write_raw(&wavEncoder, chunkSize, CYBERFM_OFFSET_PTR(pFile->pData, pFile->cursor));
            }
        } else {
            cyberfm_file_seek(pFile, chunkSize, SEEK_CUR);
            cyberfm_file_seek(pFile, (chunkSize % 2), SEEK_CUR);
        }
    }

    /* Make sure we close the WAV encoder. */
    if (hasEncoderBeenInitializd) {
        drwav_uninit(&wavEncoder);
    }

    if (pData != NULL) {
        *ppData      = pData;
        *pDataSize   = dataSize;
        *pDataFormat = fmt_customFormatCode;
    } else {
        *ppData      = NULL;
        *pDataSize   = 0;
        *pDataFormat = 0;
    }

    return CYBERFM_SUCCESS;
}
