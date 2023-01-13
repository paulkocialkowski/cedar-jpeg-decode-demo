#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <memoryAdapter.h>

struct nv12_color {
	unsigned char y;
	unsigned char u;
	unsigned char v;
};

static struct nv12_color colors[] = {
	{ 104, 128, 128 },	/* 40% gray */
	{ 180, 128, 128 },	/* 75% white */
	{ 168, 44, 136 },	/* 75% cyan */
	{ 133, 63, 52 },	/* 75% green */
	{ 63, 193, 204 },	/* 75% magenta */
	{ 51, 109, 212 },	/* 75% red */
	{ 28, 212, 120 },	/* 75% blue */
	{ 16, 128, 128 },	/* 75% black */
};

static unsigned int colors_count = sizeof(colors) / sizeof(*colors);

void test_pattern_step(unsigned int width, unsigned int height, unsigned int step, void *luma, void *chroma)
{
	unsigned int box_height = 50;
	unsigned int box_y = ((step * 2) % (height - box_height));
	unsigned int x, y, i;
	unsigned char *l, *c;
	unsigned int color_width = width / colors_count;

	l = luma;
	c = chroma;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned int index = x / color_width;
			struct nv12_color color = colors[index];

			if (y >= box_y && y < (box_y + box_height)) {
				color.y = 255 - color.y;
				color.u = 255 - color.u;
				color.v = 255 - color.v;
			}

			*l++ = color.y;

			if (!(y % 2) && !(x % 2)) {
				*c++ = color.u;
				*c++ = color.v;
			}
		}
	}
}

int main(int argc, char *argv[])
{
	VideoEncoder *encoder = NULL;
	VencBaseConfig base_config;
	VencH264Param h264_param;
	VencAllocateBufferParam buffer_param;
	VencInputBuffer input_buffer;
	VencOutputBuffer output_buffer;
	unsigned int pts = 0;
	unsigned int vbv_size;
	int ret;

	base_config.memops = MemAdapterGetOpsS();
	if (!base_config.memops) {
		fprintf(stderr, "failed to get memory adapter op\n");
		return 1;
	}

	CdcMemOpen(base_config.memops);

	/* VideoEncCreate */

	encoder = VideoEncCreate(VENC_CODEC_H264);

	/* VideoEncSetParameter: VENC_IndexParamH264Param */

	memset(&h264_param, 0, sizeof(h264_param));
	h264_param.bEntropyCodingCABAC = 1;
	h264_param.nBitrate = 512000;
	h264_param.nFramerate = 25;
	h264_param.nCodingMode = VENC_FRAME_CODING;
	h264_param.nMaxKeyInterval = 15;
	h264_param.sProfileLevel.nProfile = VENC_H264ProfileBaseline;
	h264_param.sProfileLevel.nLevel = VENC_H264Level31;
	h264_param.sQPRange.nMinqp = 10;
	h264_param.sQPRange.nMaxqp = 50;   
	h264_param.bLongRefEnable = 1;
	h264_param.nLongRefPoc = 0;

	VideoEncSetParameter(pVideoEnc, VENC_IndexParamH264Param, &h264_param);

	/* VideoEncSetParameter: VENC_IndexParamSetVbvSize */

	vbv_size = 1024 * 1024;
	VideoEncSetParameter(encoder, VENC_IndexParamSetVbvSize, &vbv_size);

	/* VideoEncInit */

	memset(&base_config, 0, sizeof(base_config));
	base_config.eInputFormat = VENC_PIXEL_YUV420SP;
	base_config.nInputWidth = 1280;
	base_config.nInputHeight = 720;
	base_config.nStride = base_config.nInputWidth;
	base_config.nDstWidth = 1280;
	base_config.nDstHeight = 720;

	VideoEncInit(encoder, &base_config);

	/* AllocInputBuffer */

	memset(&buffer_param, 0, sizeof(buffer_param));
	buffer_param.nSizeY = base_config.nStride * base_config.nInputHeight;
	buffer_param.nSizeC = buffer_param.nSizeY / 2;
	buffer_param.nBufferNum = 1;

	ret = AllocInputBuffer(encoder, &buffer_param);
	if (ret) {
		fprintf(stderr, "failed to alloc input buffer\n");
		return 1;
	}

	/* GetOneAllocInputBuffer */

	memset(&input_buffer, 0, sizeof(input_buffer));

	ret = GetOneAllocInputBuffer(encoder, &input_buffer);
	if (ret) {
		fprintf(stderr, "failed to get input buffer\n");
		return 1;
	}

	/* Draw */

	test_pattern_step(base_config.nInputWidth, base_config.nInputHeight, 0, input_buffer.pAddrVirY, input_buffer.pAddrVirC);

	/* AddOneInputBuffer */

	FlushCacheAllocInputBuffer(encoder, &input_buffer);

	input_buffer.nPts = pts;
	pts += 1000 / h264_param.nFramerate;

	AddOneInputBuffer(encoder, &input_buffer);

	/* VideoEncodeOneFrame */

	ret = VideoEncodeOneFrame(encoder);
	if (ret != VENC_RESULT_OK) {
		fprintf(stderr, "failed to encode frame\n");
		return 1;
	}

	return 0;
}
