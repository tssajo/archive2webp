/*
    Recompress a JPEG file while attempting to keep visual quality the same
    by using structural similarity (SSIM) as a metric. Does a binary search
    between JPEG quality 1 and 99 to find the best match. Also makes sure
    that huffman tables are optimized if they weren't already.
*/

#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#include "src/edit.h"
#include "src/iqa/include/iqa.h"
#include "src/smallfry.h"
#include "src/util.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

const char *COMMENT = "Compressed by jpeg-recompress";
const int minDelta = 10;

// Comparison method
enum METHOD {
    UNKNOWN,
    SSIM,
    MS_SSIM,
    SMALLFRY,
    MPE
};

const char methodName[5][9] = {
    "",
    "ssim",
    "ms-ssim",
    "smallfry",
    "mpe"
};

int method = SSIM;

// Number of binary search steps
int attempts = 8;

// Target quality (SSIM) value
enum QUALITY_PRESET {
    LOW,
    MEDIUM,
    HIGH,
    VERYHIGH
};

float target = 0;
int preset = MEDIUM;

// Min/max JPEG quality
int qMin = 1;
int qMax = 99;

// Strip metadata from the file?
int strip = 0;

// Disable progressive mode?
int noProgressive = 0;

// Defish the image?
float defishStrength = 0.0;
float defishZoom = 1.0;

// Input format
enum filetype inputFiletype = FILETYPE_AUTO;

// Whether to copy files that cannot be compressed
int copyFiles = 1;

// Whether to favor accuracy over speed
int accurate = 0;

// Chroma subsampling method
int subsample = SUBSAMPLE_DEFAULT;

// Quiet mode (less output)
int quiet = 0;

static enum QUALITY_PRESET parseQuality(const char *s) {
    if (!strcmp("low", s))
        return LOW;
    else if (!strcmp("medium", s))
        return MEDIUM;
    else if (!strcmp("high", s))
        return HIGH;
    else if (!strcmp("veryhigh", s))
        return VERYHIGH;

    error("unknown quality preset: %s", s);
    return MEDIUM;
}

static enum METHOD parseMethod(const char *s) {
    if (!strcmp("ssim", s))
        return SSIM;
    else if (!strcmp("ms-ssim", s))
        return MS_SSIM;
    else if (!strcmp("smallfry", s))
        return SMALLFRY;
    else if (!strcmp("mpe", s))
        return MPE;
    return UNKNOWN;
}

static enum filetype parseInputFiletype(const char *s) {
    if (!strcmp("auto", s))
        return FILETYPE_AUTO;
    if (!strcmp("jpeg", s))
        return FILETYPE_JPEG;
    if (!strcmp("ppm", s))
        return FILETYPE_PPM;
    return FILETYPE_UNKNOWN;
}

static void setTargetFromPreset() {
    switch (method) {
        case SSIM:
            switch (preset) {
                case LOW:
                    target = (float)0.999;
                    break;
                case MEDIUM:
                    target = (float)0.9999;
                    break;
                case HIGH:
                    target = (float)0.99995;
                    break;
                case VERYHIGH:
                    target = (float)0.99999;
                    break;
            }
            break;
        case MS_SSIM:
            switch (preset) {
                case LOW:
                    target = (float)0.85;
                    break;
                case MEDIUM:
                    target = (float)0.94;
                    break;
                case HIGH:
                    target = (float)0.96;
                    break;
                case VERYHIGH:
                    target = (float)0.98;
                    break;
            }
            break;
        case SMALLFRY:
            switch (preset) {
                case LOW:
                    target = (float)100.75;
                    break;
                case MEDIUM:
                    target = (float)102.25;
                    break;
                case HIGH:
                    target = (float)103.8;
                    break;
                case VERYHIGH:
                    target = (float)105.5;
                    break;
            }
            break;
        case MPE:
            switch (preset) {
                case LOW:
                    target = (float)1.5;
                    break;
                case MEDIUM:
                    target = (float)1.0;
                    break;
                case HIGH:
                    target = (float)0.8;
                    break;
                case VERYHIGH:
                    target = (float)0.6;
                    break;
            }
            break;
    }
}

static int parseSubsampling(const char *s) {
    if (!strcmp("default", s))
        return SUBSAMPLE_DEFAULT;
    else if (!strcmp("disable", s))
        return SUBSAMPLE_444;

    error("unknown sampling method: %s", s);
    return SUBSAMPLE_DEFAULT;
}

// Open a file for writing
FILE *openOutput(char *name) {
    if (strcmp("-", name) == 0) {
        #ifdef _WIN32
            setmode(fileno(stdout), O_BINARY);
        #endif

        return stdout;
    } else {
        return fopen(name, "wb");
    }
}

// Logs an informational message, taking quiet mode into account
void info(const char *format, ...) {
    va_list argptr;

    if (!quiet) {
        va_start(argptr, format);
        vfprintf(stderr, format, argptr);
        va_end(argptr);
    }
}

void usage(void) {
    printf("usage: %s [options] input.jpg output.jpg\n\n", progname);
    printf("options:\n\n");
    printf("  -V, --version                output program version\n");
    printf("  -h, --help                   output program help\n");
    printf("  -t, --target [arg]           set target quality [0.9999]\n");
    printf("  -q, --quality [arg]          set a quality preset: low, medium, high, veryhigh [medium]\n");
    printf("  -n, --min [arg]              minimum JPEG quality [1]\n");
    printf("  -x, --max [arg]              maximum JPEG quality [99]\n");
    printf("  -l, --loops [arg]            set the number of runs to attempt [8]\n");
    printf("  -a, --accurate               favor accuracy over speed\n");
    printf("  -m, --method [arg]           set comparison method to one of 'mpe', 'ssim', 'ms-ssim', 'smallfry' [ssim]\n");
    printf("  -s, --strip                  strip metadata\n");
    printf("  -d, --defish [arg]           set defish strength [0.0]\n");
    printf("  -z, --zoom [arg]             set defish zoom [1.0]\n");
    printf("  -r, --ppm                    parse input as PPM\n");
    printf("  -c, --no-copy                disable copying files that will not be compressed\n");
    printf("  -p, --no-progressive         disable progressive encoding\n");
    printf("  -S, --subsample [arg]        set subsampling method to one of 'default', 'disable' [default]\n");
    printf("  -T, --input-filetype [arg]   set input file type to one of 'auto', 'jpeg', 'ppm' [auto]\n");
    printf("  -Q, --quiet                  only print out errors\n");
}

int copyFile(char *outputPath, unsigned char *buf, long bufSize) {
    FILE *file = openOutput(outputPath);
    if (file == NULL) {
        error("could not open output file: %s", outputPath);
        return 1;
    }
    int wSize = fwrite(buf, bufSize, 1, file);
    if (wSize != 1) {
        fclose(file);
        error("could not write to output file: %s", outputPath);
        return 1;
    }
    int err = fclose(file);
    if (err) {
        error("could not close the output file: %s", outputPath);
        return 1;
    }
    return 0;
}

int main (int argc, char **argv) {
    const char *optstring = "Vht:q:n:x:l:am:sd:z:rcpS:T:Q";
    static const struct option opts[] = {
        { "version", no_argument, 0, 'V' },
        { "help", no_argument, 0, 'h' },
        { "target", required_argument, 0, 't' },
        { "quality", required_argument, 0, 'q' },
        { "min", required_argument, 0, 'n' },
        { "max", required_argument, 0, 'x' },
        { "loops", required_argument, 0, 'l' },
        { "accurate", no_argument, 0, 'a' },
        { "method", required_argument, 0, 'm' },
        { "strip", no_argument, 0, 's' },
        { "defish", required_argument, 0, 'd' },
        { "zoom", required_argument, 0, 'z' },
        { "ppm", no_argument, 0, 'r' },
        { "no-copy", no_argument, 0, 'c' },
        { "no-progressive", no_argument, 0, 'p' },
        { "subsample", required_argument, 0, 'S' },
        { "input-filetype", required_argument, 0, 'T' },
        { "quiet", no_argument, 0, 'Q' },
        { 0, 0, 0, 0 }
    };
    int opt, longind = 0;

    progname = "jpeg-recompress";

    while ((opt = getopt_long(argc, argv, optstring, opts, &longind)) != -1) {
        switch (opt) {
        case 'V':
            version();
            return 0;
        case 'h':
            usage();
            return 0;
        case 't':
            target = atof(optarg);
            break;
        case 'q':
            preset = parseQuality(optarg);
            break;
        case 'n':
            qMin = atoi(optarg);
            break;
        case 'x':
            qMax = atoi(optarg);
            break;
        case 'l':
            attempts = atoi(optarg);
            break;
        case 'a':
            accurate = 1;
            break;
        case 'm':
            method = parseMethod(optarg);
            break;
        case 's':
            strip = 1;
            break;
        case 'd':
            defishStrength = atof(optarg);
            break;
        case 'z':
            defishZoom = atof(optarg);
            break;
        case 'r':
            inputFiletype = FILETYPE_PPM;
            break;
        case 'c':
            copyFiles = 0;
            break;
        case 'p':
            noProgressive = 1;
            break;
        case 'S':
            subsample = parseSubsampling(optarg);
            break;
        case 'T':
            if (inputFiletype != FILETYPE_AUTO) {
                error("multiple file types specified for the input file");
                return 1;
            }
            inputFiletype = parseInputFiletype(optarg);
            break;
        case 'Q':
            quiet = 1;
            break;
        };
    }

    if (argc - optind != 2) {
        usage();
        return 255;
    }

    if (method == UNKNOWN) {
        error("invalid method!");
        usage();
        return 255;
    }

    if (qMin > qMax) {
        error("maximum JPEG quality must not be smaller than minimum JPEG quality!");
        return 1;
    }

    // No target passed, use preset!
    if (!target) {
        setTargetFromPreset();
    }

    unsigned char *buf;
    long bufSize = 0;
    unsigned char *original;
    long originalSize = 0;
    unsigned char *originalGray = NULL;
    long originalGraySize = 0;
    unsigned char *compressed = NULL;
    unsigned long compressedSize = 0;
    unsigned long totalSize = 0;
    unsigned char *compressedGray;
    long compressedGraySize = 0;
    unsigned char *tmpImage;
    int width, height;
    unsigned char *metaBuf = NULL;
    unsigned int metaSize = 0;
    unsigned int metaSizeCOM = strlen(COMMENT) + 4;
    FILE *file;
    char *inputPath = argv[optind];
    char *outputPath = argv[optind + 1];

    /* Read the input into a buffer. */
    bufSize = readFile(inputPath, (void **) &buf);
    if (!bufSize) {
        return 1;
    }

    /* Detect input file type. */
    if (inputFiletype == FILETYPE_AUTO)
        inputFiletype = detectFiletypeFromBuffer(buf, bufSize);

    /*
     * Read original image and decode. We need the raw buffer contents and its
     * size to obtain meta data and the original file size later.
     */
    originalSize = decodeFileFromBuffer(buf, bufSize, &original, inputFiletype, &width, &height, JCS_RGB);
    if (!originalSize) {
        error("invalid input file: %s", inputPath);
        free(buf);
        return 1;
    }

    if (defishStrength) {
        info("Defishing...\n");
        tmpImage = malloc(width * height * 3);
        defish(original, tmpImage, width, height, 3, defishStrength, defishZoom);
        free(original);
        original = tmpImage;
    }

    // Convert RGB input into Y
    originalGraySize = grayscale(original, &originalGray, width, height);
    if (!originalGraySize) {
        free(original);
        free(buf);
        return 1;
    }

    if (inputFiletype == FILETYPE_JPEG) {
        // Read metadata (EXIF / IPTC / XMP tags)
        if (getMetadata(buf, bufSize, &metaBuf, &metaSize, COMMENT)) {

            free(originalGray);
            free(original);

            if (copyFiles) {
                info("File already processed by jpeg-recompress!\n");

                copyFile(outputPath, buf, bufSize);

                free(buf);
                return 0;
            } else {
                error("file already processed by jpeg-recompress!");
                free(buf);
                return 2;
            }
        }
    }

    if (strip) {
        // Pretend we have no metadata
        metaSize = 0;
        if (metaBuf != NULL) {
            free(metaBuf);
            metaBuf = NULL;
        }
    } else {
        info("Metadata size is %ukb\n", metaSize / 1024);
    }

    // Do a binary search to find the optimal encoding quality for the
    // given target SSIM value.
    float newDiff;
    float bestDiff = FLT_MAX;
    int bestQuality = INT_MIN;
    int quality;
    int min = qMin, max = qMax;
    for (int attempt = attempts - 1; attempt >= 0; --attempt) {
        quality = (min + max) / 2;

        // We were already at this quality level? If yes then let's make this
        // the final run and the final image will be processed optimized this time
        if (quality == bestQuality)
            attempt = 0;

        // Terminate early once bisection interval is a singleton.
        if (min == max)
            attempt = 0;

        int progressive = attempt ? 0 : !noProgressive;
        int optimize = accurate ? 1 : (attempt ? 0 : 1);

        // If compressed buffer is not empty the free it
        if (compressed != NULL) {
            free(compressed);
            compressed = NULL;
        }

        // Recompress to a new quality level, without optimizations (for speed)
        compressedSize = encodeJpeg(&compressed, original, width, height, JCS_RGB, quality, progressive, optimize, subsample);

        // Load compressed luma for quality comparison
        compressedGraySize = decodeJpeg(compressed, compressedSize, &compressedGray, &width, &height, JCS_GRAYSCALE);
        if (!compressedGraySize) {
            error("unable to decode file that was just encoded!");

            free(compressed);
            if (metaBuf != NULL)
                free(metaBuf);
            free(originalGray);
            free(original);
            free(buf);

            return 1;
        }

        // Measure quality difference
        float metric;
        switch (method) {
            case MS_SSIM:
                metric = iqa_ms_ssim(originalGray, compressedGray, width, height, width, 0);
                break;
            case SMALLFRY:
                metric = smallfry_metric(originalGray, compressedGray, width, height);
                break;
            case MPE:
                metric = meanPixelError(originalGray, compressedGray, width, height, 1);
                break;
            case SSIM: default:
                metric = iqa_ssim(originalGray, compressedGray, width, height, width, 0, 0);
                break;
        }

        // We no longer need compressedGray
        free(compressedGray);

        newDiff = fabs(target - metric);
        if (newDiff < bestDiff) {
            bestDiff = newDiff;
            bestQuality = quality;
        }

        if (attempt) {
            info("%s at q=%i (%i - %i): %f (target is %f difference is %f)\n", methodName[method], quality, min, max, metric, target, newDiff);
        } else {
            info("Final optimized %s at q=%i (%i - %i): %f (target was %f, difference is %f)\n", methodName[method], quality, min, max, metric, target, newDiff);
        }

        totalSize = compressedSize + metaSizeCOM + metaSize;
        if (metric < target) {
            if (totalSize + minDelta >= bufSize) {
                free(compressed);
                if (metaBuf != NULL)
                    free(metaBuf);
                free(originalGray);
                free(original);

                if (copyFiles) {
                    info("Output file would be larger than input!\n");

                    copyFile(outputPath, buf, bufSize);

                    free(buf);
                    return 0;
                } else {
                    error("output file would be larger than input!");
                    free(buf);
                    return 1;
                }
            }

            switch (method) {
                case SSIM: case MS_SSIM: case SMALLFRY:
                    // Too distorted, increase quality
                    min = MIN(quality + 1, max);
                    break;
                case MPE:
                    // Higher than required, decrease quality
                    max = MAX(quality - 1, min);
                    break;
            }
        } else {
            switch (method) {
                case SSIM: case MS_SSIM: case SMALLFRY:
                    // Higher than required, decrease quality
                    max = MAX(quality - 1, min);
                    break;
                case MPE:
                    // Too distorted, increase quality
                    min = MIN(quality + 1, max);
                    break;
            }
        }

    }

    free(originalGray);
    free(original);

    // Calculate and show savings, if any
    int percent = totalSize * 100 / bufSize;
    unsigned long saved = (bufSize > totalSize) ? bufSize - totalSize : 0;
    info("New size is %i%% of original (saved %lu kb)\n", percent, saved / 1024);

    if (totalSize >= bufSize) {
        error("output file is larger than input, aborting!");

        copyFile(outputPath, buf, bufSize);

        free(compressed);
        if (metaBuf != NULL)
            free(metaBuf);
        free(buf);

        return 1;
    }

    free(buf);

    /* Check that the metadata starts with a SOI marker. */
    if (!checkJpegMagic(compressed, compressedSize)) {
        error("missing SOI marker, aborting!");

        free(compressed);
        if (metaBuf != NULL)
            free(metaBuf);

        return 1;
    }

    /* Make sure APP0 is recorded immediately after the SOI marker. */
    if (compressed[2] != 0xff || compressed[3] != 0xe0) {
        error("missing APP0 marker, aborting!");

        free(compressed);
        if (metaBuf != NULL)
            free(metaBuf);

        return 1;
    }

    // Open output file for writing
    file = openOutput(outputPath);
    if (file == NULL) {
        error("could not open output file: %s", outputPath);

        free(compressed);
        if (metaBuf != NULL)
            free(metaBuf);

        return 1;
    }

    /* Write SOI marker and APP0 metadata to the output file. */
    unsigned int app0_lenP4 = 4 + (compressed[4] << 8) + compressed[5];
    int wSize = fwrite(compressed, app0_lenP4, 1, file);
    if (wSize != 1) {
        fclose(file);
        error("could not write to output file: %s", outputPath);

        free(compressed);
        if (metaBuf != NULL)
            free(metaBuf);

        return 1;
    }

    /*
     * Write comment (COM metadata) so we know not to reprocess this file in
     * the future if it gets passed in again.
     */
    int comLen = strlen(COMMENT);
    fputc(0xff, file);
    fputc(0xfe, file);
    fputc(0x00, file);
    fputc(comLen + 2, file);
    wSize = fwrite(COMMENT, comLen, 1, file);
    if (wSize != 1) {
        fclose(file);
        error("could not write to output file: %s", outputPath);

        free(compressed);
        if (metaBuf != NULL)
            free(metaBuf);

        return 1;
    }

    /* Write additional metadata markers. */
    if (metaBuf != NULL) {
        wSize = fwrite(metaBuf, metaSize, 1, file);

        free(metaBuf);

        if (wSize != 1) {
            fclose(file);
            error("could not write to output file: %s", outputPath);

            free(compressed);

            return 1;
        }
    }

    /* Write image data. */
    wSize = fwrite(compressed + app0_lenP4, compressedSize - app0_lenP4, 1, file);

    free(compressed);

    if (wSize != 1) {
        fclose(file);
        error("could not write to output file: %s", outputPath);

        return 1;
    }

    int err = fclose(file);
    if (err) {
        error("could not close the output file: %s", outputPath);
        return 1;
    }

    return 0;
}
