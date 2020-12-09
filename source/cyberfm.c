/*
Public domain.

David Reid - mackron@gmail.com
*/
#include "libcyberfm.c"
#include <stdio.h>

#define AUDIO_OUTPUT_PATH   "output/audio2"

static cyberfm_result cyberfm_argv_find(int argc, const char** argv, const char* key, int* pIndexOut)
{
    int i;

    /* Always start at 1 because 0 is the name of the executable. */
    if (argc <= 1 || argv == NULL || key == NULL) {
        return CYBERFM_INVALID_ARGS;
    }

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], key) == 0) {
            if (pIndexOut != NULL) {
                *pIndexOut = i;
            }
            return CYBERFM_SUCCESS;  /* Found. */
        }
    }

    /* Not found. */
    return CYBERFM_DOES_NOT_EXIST;
}

cyberfm_bool32 cyberfm_argv_is_set(int argc, char** argv, const char* key)
{
    cyberfm_result result = cyberfm_argv_find(argc, argv, key, NULL);
    if (result != CYBERFM_SUCCESS) {
        return CYBERFM_FALSE;    /* Key not found. */
    }

    return CYBERFM_TRUE;
}

const char* cyberfm_argv_get_value(int argc, char** argv, const char* key)
{
    cyberfm_result result;
    int keyIndex;
    const char* value;

    result = cyberfm_argv_find(argc, argv, key, &keyIndex);
    if (result != CYBERFM_SUCCESS) {
        return NULL;
    }

    if (keyIndex+1 == argc) {
        return NULL;    /* Nothing after the key. */
    }

    value = argv[keyIndex+1];
    if (value[0] == '-' || value[0] == '+') {
        return NULL;    /* Looks like another command line switch comes after. */
    }
    
    return value;
}



int main(int argc, char** argv)
{
    cyberfm_result result;
    cyberfm_archive archive;
    char outputDir[256];
    uint32_t iFile;

    if (argc < 2) {
        printf("No input file specified.");
        return 0;
    }

    /* If we're extracting, extract every archive on the command line. */
    if (cyberfm_argv_is_set(argc, argv, "--extract")) {
        int iarg;

        for (iarg = 1; iarg < argc; iarg += 1) {
            const char* pArchivePath = argv[iarg];

            if (mfs_file_exists(pArchivePath)) {
                const char* pCmdLineOutputDir;

                result = cyberfm_archive_init(pArchivePath, &archive);
                if (result != CYBERFM_SUCCESS) {
                    printf("Failed to open archive \"%s\".", argv[1]);
                    return -1;
                }

                /* Extract the entire archive to the specified output directory. */
                pCmdLineOutputDir = cyberfm_argv_get_value(argc, argv, "-o");
                if (pCmdLineOutputDir != NULL) {
                    /* The output directory is set. */
                    mfs_path_copy(outputDir, sizeof(outputDir), pCmdLineOutputDir, NULL);
                } else {
                    /* The output directory is not set. Output to a directory with the same name as the archive, minus the extension. */
                    mfs_path_remove_extension(outputDir, sizeof(outputDir), pArchivePath, NULL);
                }

                /* Make sure the output directory exists. */
                if (mfs_mkdir(outputDir, MFS_TRUE) != MFS_SUCCESS) {
                    printf("Failed to create directory: %s\n", outputDir);
                }

                /* Now we can extract the files. When there's only a single sub-file we'll just output the file directly. Otherwise we'll create a folder. */
                for (iFile = 0; iFile < archive.pCentralDirectory->fileInfoCount; iFile += 1) {
                    cyberfm_file* pFile;
            
                    printf("Extracting %u/%u: %llu", iFile + 1, archive.pCentralDirectory->fileInfoCount, archive.pCentralDirectory->pFileInfo[iFile].hashedName);

                    if ((archive.pCentralDirectory->pFileInfo[iFile].dataSpecRangeEnd - archive.pCentralDirectory->pFileInfo[iFile].dataSpecRangeBeg) > 1) {
                        /* Output to a folder. */
                        uint32_t iSubFile;
                        char fileDir[256];

                        /* First make sure the folder exists. */
                        snprintf(fileDir, sizeof(fileDir), "%s/%llu", outputDir, archive.pCentralDirectory->pFileInfo[iFile].hashedName);
                        mfs_mkdir(fileDir, MFS_TRUE);
                
                        for (iSubFile = archive.pCentralDirectory->pFileInfo[iFile].dataSpecRangeBeg; iSubFile < archive.pCentralDirectory->pFileInfo[iFile].dataSpecRangeEnd; iSubFile += 1) {
                            char subFilePath[256];
                            snprintf(subFilePath, sizeof(subFilePath), "%s/%u", fileDir, iSubFile - archive.pCentralDirectory->pFileInfo[iFile].dataSpecRangeBeg);

                            result = cyberfm_file_open_by_index(&archive, iFile, iSubFile - archive.pCentralDirectory->pFileInfo[iFile].dataSpecRangeBeg, &pFile);
                            if (result != CYBERFM_SUCCESS) {
                                printf(". Failed to open file");
                                continue;
                            }

                            result = cyberfm_result_from_minifs(mfs_open_and_write_file(subFilePath, pFile->size, pFile->pData));
                            if (result != CYBERFM_SUCCESS) {
                                cyberfm_file_close(pFile);
                                printf(". Failed to extract file");
                                continue;
                            }

                            /* Extraction complete. */
                            cyberfm_file_close(pFile);
                        }
                    } else {
                        /* Output the file directly. */
                        char subFilePath[256];
                        snprintf(subFilePath, sizeof(subFilePath), "%s/%llu", outputDir, archive.pCentralDirectory->pFileInfo[iFile].hashedName);

                        result = cyberfm_file_open_by_index(&archive, iFile, 0, &pFile);
                        if (result != CYBERFM_SUCCESS) {
                            printf(". Failed to open file");
                            continue;
                        }

                        /* TODO: Later on once we've figured out the compression stuff we'll want to change this. */
                        result = cyberfm_result_from_minifs(mfs_open_and_write_file(subFilePath, pFile->size, pFile->pData));
                        if (result != CYBERFM_SUCCESS) {
                            cyberfm_file_close(pFile);
                            printf(". Failed to extract file");
                            continue;
                        }

                        /* Extraction complete. */
                        cyberfm_file_close(pFile);
                    }

                    printf("\n");
                }

                cyberfm_archive_uninit(&archive);
            } else {
                /* As soon as we hit an argument that's not a file, end iterating. */
                break;
            }
        }
    }


#if 0
    /* TESTING: Output all audio files. */

    /* Make sure the output folder exists first. */
    mfs_mkdir(AUDIO_OUTPUT_PATH, MFS_TRUE);

    /* Now loop over every file and try processing them all. */
    for (iFile = 0; iFile < archive.pCentralDirectory->fileInfoCount; iFile += 1) {
        cyberfm_file* pFile;
        void* pAudioData;
        size_t audioDataSize;
        int audioFormat;
        char filePath[256];
        
        result = cyberfm_file_open_by_index(&archive, iFile, 0, &pFile);
        if (result != CYBERFM_SUCCESS) {
            printf("WARNING: Failed to open file at index %u.\n", iFile);
            continue;
        }

        if (archive.pCentralDirectory->pFileInfo[iFile].hashedName == 30289915656255236) {
            int a = 6; (void)a;
        }

        result = cyberfm_file_extract_audio(pFile, &pAudioData, &audioDataSize, &audioFormat);
        if (result != CYBERFM_SUCCESS) {
            continue;   /* Probably not an audio file. */
        }

        /* The only thing we're doing different depending on the type is the file name. */
        if (audioFormat == CYBERFM_AUDIO_FORMAT_PCM) {
            snprintf(filePath, sizeof(filePath), "%s/%llu.wav", AUDIO_OUTPUT_PATH, archive.pCentralDirectory->pFileInfo[iFile].hashedName);
        } else if (audioFormat == CYBERFM_AUDIO_FORMAT_OPUS) {
            snprintf(filePath, sizeof(filePath), "%s/%llu.opus", AUDIO_OUTPUT_PATH, archive.pCentralDirectory->pFileInfo[iFile].hashedName);
        } else {
            free(pAudioData);
            cyberfm_file_close(pFile);
            continue;   /* Unknown audio data type. */
        }

        result = cyberfm_result_from_minifs(mfs_open_and_write_file(filePath, audioDataSize, pAudioData));
        if (result != CYBERFM_SUCCESS) {
            printf("WARNING: Failed to write audio file \"%s\".\n", filePath);
        }

        free(pAudioData);
        cyberfm_file_close(pFile);
    }
#endif

    return 0;
}
