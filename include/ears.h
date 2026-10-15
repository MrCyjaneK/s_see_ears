#ifndef SPACESEE_EARS_H
#define SPACESEE_EARS_H

#ifdef __cplusplus
extern "C" {
#endif

int SPACESEE_ears_load(const char *workDir, const char *assetsDir);
const char *SPACESEE_ears_loadError(void);
void SPACESEE_ears_unload(void);
int SPACESEE_ears_isHeavy(void);
int SPACESEE_ears_assetCount(void);
const char *SPACESEE_ears_asset(int index);
const char *SPACESEE_ears_version(void);
const char *SPACESEE_ears_extension(void);
const char *SPACESEE_ears_filetypes(void);
long long SPACESEE_ears_maxInputSize(void);
long long SPACESEE_ears_analyze(const char *inputPath, const char *outputPath, const char *dataDir);
int SPACESEE_ears_progress(long long jobId, int scale);
const char *SPACESEE_ears_progressString(long long jobId);
void SPACESEE_ears_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
