#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>

#include <memoryAdapter.h>
#include <vdecoder.h>

#define CEDAR_OUTPUT_PREFIX	"./"

#define time_diff(tb, ta) \
       ((ta.tv_sec * 1000000000UL + ta.tv_nsec) - (tb.tv_sec * 1000000000UL + tb.tv_nsec))

VideoPicture *picture_decode_tiled(VideoDecoder *decoder,
				   VideoStreamDataInfo *data_info)
{
	VideoPicture *picture_tiled;
	struct timespec ts_before, ts_after;
	uint64_t ts_diff;
	int ret;

	ret = SubmitVideoStreamData(decoder, data_info, 0);
	if (ret) {
		fprintf(stderr, "failed to submit video buffer\n");
		goto error;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_before);

	ret = DecodeVideoStream(decoder, 0, 0, 0, 0);
	switch (ret) {
	case VDECODE_RESULT_KEYFRAME_DECODED:
	case VDECODE_RESULT_FRAME_DECODED:
		fprintf(stderr, "Decoded frame!\n");
		break;
	case VDECODE_RESULT_OK:
		fprintf(stderr, "OK frame!\n");
		break;
	case VDECODE_RESULT_NO_FRAME_BUFFER:
		fprintf(stderr, "Decode missing frame buffer!\n");
		goto error;
	default:
		fprintf(stderr, "Decode error: %d\n", ret);
		goto error;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_after);

	ts_diff = time_diff(ts_before, ts_after) / 1000;
	printf("Decode time: %"PRIu64" us\n", ts_diff);

	picture_tiled = RequestPicture(decoder, 0);
	if (!picture_tiled) {
		fprintf(stderr, "failed to get picture!\n");
		goto error;
	}

	printf("Decoded picture %x: %ux%u at virt addr %x/%x, phy addr %#x/%#x\n", picture_tiled, picture_tiled->nWidth, picture_tiled->nHeight, picture_tiled->pData0, picture_tiled->pData1, MemAdapterGetPhysicAddress(picture_tiled->pData0), MemAdapterGetPhysicAddress(picture_tiled->pData1));

	printf("Decoded picture pixel format: ");

	switch (picture_tiled->ePixelFormat) {
	case PIXEL_FORMAT_YUV_PLANER_420:
		printf("YUV 4:2:0 planar\n");
		break;
	case PIXEL_FORMAT_YUV_PLANER_422:
		printf("YUV 4:2:2 planar\n");
		break;
	case PIXEL_FORMAT_YUV_PLANER_444:
		printf("YUV 4:4:4 planar\n");
		break;
	case PIXEL_FORMAT_YUV_MB32_420:
		printf("YUV 4:2:0 MB-32 tiled\n");
		break;
	case PIXEL_FORMAT_YUV_MB32_422:
		printf("YUV 4:2:2 MB-32 tiled\n");
		break;
	case PIXEL_FORMAT_YUV_MB32_444:
		printf("YUV 4:2:2 MB-32 tiled\n");
		break;
	case PIXEL_FORMAT_DEFAULT:
	default:
		printf("unknown\n");
		break;
	}

	return picture_tiled;

error:
	return NULL;
}

VideoPicture *picture_untile(VideoPicture *picture_tiled)
{
	VideoPicture *picture;
	struct timespec ts_before, ts_after;
	uint64_t ts_diff;
	unsigned int luma_size;
	unsigned int chroma_size;

	luma_size = picture_tiled->nWidth * picture_tiled->nHeight;
	chroma_size = picture_tiled->nWidth * picture_tiled->nHeight / 2;

	clock_gettime(CLOCK_MONOTONIC, &ts_before);

	picture = calloc(1, sizeof(*picture));
	picture->ePixelFormat = PIXEL_FORMAT_NV12;
	picture->pData0 = malloc(luma_size + chroma_size);

	MemAdapterFlushCache(picture_tiled->pData0, luma_size);
	MemAdapterFlushCache(picture_tiled->pData1, chroma_size);

	ConvertPixelFormat(picture_tiled, picture);

	clock_gettime(CLOCK_MONOTONIC, &ts_after);

	ts_diff = time_diff(ts_before, ts_after) / 1000;
	printf("Untile time: %"PRIu64" us\n", ts_diff);

	return picture;
}

void picture_untile_free(VideoPicture *picture)
{
	free(picture->pData0);
	free(picture);
}


int main(int argc, char *argv[])
{
	VideoDecoder *decoder = NULL;
	VConfig config;
	VideoStreamInfo stream_info;
	VideoStreamDataInfo data_info;
	VideoPicture *picture_tiled;
	VideoPicture *picture;

	struct timespec ts_before, ts_after;
	struct stat input_stat;
	unsigned int input_size;
	char *input_path;
	char *input_buffer = NULL;
	unsigned int input_buffer_size = 0;
	char *ring_buffer = NULL;
	unsigned int ring_buffer_size = 0;
	unsigned int luma_size;
	unsigned int chroma_size;
	unsigned int width, height;
	int input_fd;
	int output_fd;
	int index;
	int ret;

	if (argc < 2)
		return 1;

	input_path = argv[1];

	/* Input */

	input_fd = open(input_path, O_RDONLY);
	if (input_fd < 0) {
		fprintf(stderr, "failed to open input file\n");
		return 1;
	}

	memset(&input_stat, 0, sizeof(input_stat));

	ret = fstat(input_fd, &input_stat);
	if (ret) {
		fprintf(stderr, "failed to stat input file\n");
		return 1;
	}

	input_size = input_stat.st_size;

	printf("Input file size is %u bytes\n", input_size);

	/* Output */

	output_fd = open(CEDAR_OUTPUT_PREFIX "/output.yuv", O_RDWR | O_TRUNC | O_CREAT, 0644);
	if (output_fd < 0) {
		fprintf(stderr, "failed to open output file\n");
		return 1;
	}

	decoder = CreateVideoDecoder();

	/* Picture */

	width = 1280;
	height = 720;

	memset(&config, 0, sizeof(config));
	/* Not used for JPEG. */
	config.eOutputPixelFormat = PIXEL_FORMAT_YUV_MB32_420;

	ret = MemAdapterOpen();
	if (ret < 0) {
		fprintf(stderr, "failed to open memory adapter\n");
		goto error;
	}

	memset(&stream_info, 0, sizeof(stream_info));
	stream_info.eCodecFormat = VIDEO_CODEC_FORMAT_MJPEG;
	stream_info.nWidth = width;
	stream_info.nHeight = height;

	ret = InitializeVideoDecoder(decoder, &stream_info, &config);
	if (ret) {
		fprintf(stderr, "failed to initialize video decoder\n");
		goto error;
	}

	ret = RequestVideoStreamBuffer(decoder, input_size, &input_buffer,
				       &input_buffer_size, &ring_buffer,
				       &ring_buffer_size, 0);
	if (ret) {
		fprintf(stderr, "failed to request video buffer\n");
		goto error;
	}

	printf("Allocated %u+%u bytes for input+ring buffer\n",
	       input_buffer_size, ring_buffer_size);

	if (input_buffer_size < input_size) {
		fprintf(stderr, "allocated input buffer size is too small!\n");
		goto error;
	}

	ret = read(input_fd, input_buffer, input_size);
	if (ret < input_size) {
		fprintf(stderr, "failed to read input data!\n");
		goto error;
	}

	printf("Read %d bytes from input to buffer\n", ret);

	memset(&data_info, 0, sizeof(data_info));
	data_info.pData = input_buffer;
	data_info.nLength = input_size;
	data_info.bIsFirstPart = 1;
	data_info.bIsLastPart = 1;

	/* First decode */

	picture_tiled = picture_decode_tiled(decoder, &data_info);
	if (!picture_tiled) {
		fprintf(stderr, "failed to decode picture!\n");
		goto error;
	}

	/* Second decode */

	picture_tiled = picture_decode_tiled(decoder, &data_info);
	if (!picture_tiled) {
		fprintf(stderr, "failed to decode picture!\n");
		goto error;
	}

	/* Untile */

	picture = picture_untile(picture_tiled);
	if (!picture) {
		fprintf(stderr, "failed to untile picture!\n");
		goto error;
	}

	ReturnPicture(decoder, picture_tiled);

	luma_size = picture_tiled->nWidth * picture_tiled->nHeight;
	chroma_size = picture_tiled->nWidth * picture_tiled->nHeight / 2;

	write(output_fd, picture->pData0, luma_size + chroma_size);

	picture_untile_free(picture);

	ret = 0;
	goto complete;

error:
	ret = 1;

complete:
	DestroyVideoDecoder(decoder);
	MemAdapterClose();

	close(output_fd);
	close(input_fd);

	return ret;
}
