/*
    Converts a JPEG file to WebP file format while attempting to keep visual
    quality the same by using structural similarity (SSIM) as a metric. Does
    a binary search between quality settings 1 and 99 to find the best match.
*/

#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <limits.h>

#include "../libwebp/src/webp/encode.h"
#include "../libwebp/src/webp/encode.h"

#include "src/edit.h"
#include "src/iqa/include/iqa.h"
#include "src/smallfry.h"
#include "src/util.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

const char *COMMENT = "Compressed by archive2webp";

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

// Min/max image quality
int qMin = 1;
int qMax = 99;

// Defish the image?
float defishStrength = 0.0;
float defishZoom = 1.0;

// Input format
enum filetype inputFiletype = FILETYPE_AUTO;

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
                    target = (float)0.995;
                    break;
                case MEDIUM:
                    target = (float)0.999;
                    break;
                case HIGH:
                    target = (float)0.9995;
                    break;
                case VERYHIGH:
                    target = (float)0.9999;
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
    printf("usage: %s [options] input.jpg output.webp\n\n", progname);
    printf("options:\n\n");
    printf("  -V, --version                output program version\n");
    printf("  -h, --help                   output program help\n");
    printf("  -t, --target [arg]           set target quality [0.9999]\n");
    printf("  -q, --quality [arg]          set a quality preset: low, medium, high, veryhigh [medium]\n");
    printf("  -n, --min [arg]              minimum image quality [1]\n");
    printf("  -x, --max [arg]              maximum image quality [99]\n");
    printf("  -l, --loops [arg]            set the number of runs to attempt [8]\n");
    printf("  -m, --method [arg]           set comparison method to one of 'mpe', 'ssim', 'ms-ssim', 'smallfry' [ssim]\n");
    printf("  -d, --defish [arg]           set defish strength [0.0]\n");
    printf("  -z, --zoom [arg]             set defish zoom [1.0]\n");
    printf("  -r, --ppm                    parse input as PPM\n");
    printf("  -T, --input-filetype [arg]   set input file type to one of 'auto', 'jpeg', 'ppm' [auto]\n");
    printf("  -Q, --quiet                  only print out errors\n");
}

int main (int argc, char **argv) {
    const char *optstring = "Vht:q:n:x:l:m:d:z:r:T:Q";
    static const struct option opts[] = {
        { "version", no_argument, 0, 'V' },
        { "help", no_argument, 0, 'h' },
        { "target", required_argument, 0, 't' },
        { "quality", required_argument, 0, 'q' },
        { "min", required_argument, 0, 'n' },
        { "max", required_argument, 0, 'x' },
        { "loops", required_argument, 0, 'l' },
        { "method", required_argument, 0, 'm' },
        { "defish", required_argument, 0, 'd' },
        { "zoom", required_argument, 0, 'z' },
        { "ppm", no_argument, 0, 'r' },
        { "input-filetype", required_argument, 0, 'T' },
        { "quiet", no_argument, 0, 'Q' },
        { 0, 0, 0, 0 }
    };
    int opt, longind = 0;

    progname = "archive2webp";

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
        case 'm':
            method = parseMethod(optarg);
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
        error("maximum image quality must not be smaller than minimum image quality!");
        return 1;
    }

    // No target passed, use preset!
    if (!target) {
        setTargetFromPreset();
    }

    WebPConfig config;
    // if (!WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, 50)) {
    if (!WebPConfigPreset(&config, WEBP_PRESET_PHOTO, 50)) {
        error("could not initialize WebP configuration");
        return 1;
    }
    WebPPicture pic;
    if (!WebPPictureInit(&pic)) {
        error("could not initialize WebP picture");
        return 1;
    }
    WebPMemoryWriter wrt;
    WebPMemoryWriterInit(&wrt);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = (void*)&wrt;

    unsigned char *buf;
    long bufSize = 0;
    unsigned char *original;
    long originalSize = 0;
    unsigned char *originalGray = NULL;
    long originalGraySize = 0;
    unsigned char *compressedGray;
    long compressedGraySize = 0;
    unsigned char *tmpImage;
    uint8_t *decodedImage = NULL;
    int width, height;
    FILE *file;
    char *inputPath = argv[optind];
    char *outputPath = argv[optind + 1];

    /* Read the input into a buffer. */
    bufSize = readFile(inputPath, (void **) &buf);
    if (!bufSize) {

        WebPMemoryWriterClear(&wrt);

        return 1;
    }

    /* Detect input file type. */
    if (inputFiletype == FILETYPE_AUTO)
        inputFiletype = detectFiletypeFromBuffer(buf, bufSize);

    /* Read original image and decode. */
    originalSize = decodeFileFromBuffer(buf, bufSize, &original, inputFiletype, &width, &height, JCS_RGB);

    free(buf);

    if (!originalSize) {
        error("invalid input file: %s", inputPath);

        WebPMemoryWriterClear(&wrt);

        return 1;
    }

    if (defishStrength) {
        info("Defishing...\n");
        tmpImage = malloc(width * height * 3);
        defish(original, tmpImage, width, height, 3, defishStrength, defishZoom);
        free(original);
        original = tmpImage;
    }

    // WebP image dimensions
    pic.width = width;
    pic.height = height;

    int rgb_stride = width * 3;
    int err = WebPPictureImportRGB(&pic, original, rgb_stride);
    if (!err) {
        error("could not import RGB image to WebP");

        WebPMemoryWriterClear(&wrt);
        free(original);

        return 1;
    }

    // Convert RGB input into Y
    originalGraySize = grayscale(original, &originalGray, width, height);
    free(original);
    if (!originalGraySize) {
        error("could not create the original grayscale image");

        WebPMemoryWriterClear(&wrt);
        WebPPictureFree(&pic);
        return 1;
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

        // We were already at this quality level? If yes then let's make this the final run
        if (quality == bestQuality)
            attempt = 0;

        // Terminate early once bisection interval is a singleton.
        if (min == max)
            attempt = 0;

        WebPMemoryWriterClear(&wrt);

        // Recompress to a new quality level
        config.quality = (float)quality;
        int ok = WebPEncode(&config, &pic);
        if (!ok) {
            error("could not encode image to WebP");

            WebPMemoryWriterClear(&wrt);
            WebPPictureFree(&pic); // must be called independently of the 'ok' result
            free(originalGray);

            return 1;

        }

        // Decode the just encoded buffer
        decodedImage = WebPDecodeRGB(wrt.mem, wrt.size, &width, &height);
        if (decodedImage == NULL) {
            error("unable to decode buffer that was just encoded!");

            WebPMemoryWriterClear(&wrt);
            WebPPictureFree(&pic);
            free(originalGray);

            return 1;
        }

        // Convert RGB input into Y
        compressedGraySize = grayscale(decodedImage, &compressedGray, width, height);

        // Free the decoded RGB image
        WebPFree(decodedImage);

        if (!compressedGraySize) {
            error("could not create decoded grayscale image");

            WebPMemoryWriterClear(&wrt);
            WebPPictureFree(&pic);
            free(originalGray);

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
            info("%s at q=%u (%02u - %u): %f (target: %f diff: %f) size: %u\n", methodName[method], quality, min, max, metric, target, newDiff, wrt.size);
        } else {
            info("Final optimized %s at q=%u: %f (target: %f diff: %f) size: %u\n", methodName[method], quality, metric, target, newDiff, wrt.size);
        }

        if (metric < target) {

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

    WebPPictureFree(&pic);
    free(originalGray);

    // Calculate and show savings, if any
    int percent = wrt.size * 100 / bufSize;
    unsigned long saved = (bufSize > wrt.size) ? bufSize - wrt.size : 0;
    info("New size is %i%% of original (saved %lu kb)\n", percent, saved / 1024);

    // Open output file for writing
    file = openOutput(outputPath);
    if (file == NULL) {
        error("could not open output file: %s", outputPath);

        WebPMemoryWriterClear(&wrt);

        return 1;
    }

    /* Write image data. */
    int wSize = fwrite(wrt.mem, wrt.size, 1, file);

    WebPMemoryWriterClear(&wrt);

    if (wSize != 1) {
        fclose(file);
        error("could not write to output file: %s", outputPath);

        return 1;
    }

    err = fclose(file);
    if (err) {
        error("could not close the output file: %s", outputPath);
        return 1;
    }

    return 0;
}
