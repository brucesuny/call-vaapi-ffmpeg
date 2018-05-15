/*********************************************************************
testvaapi: directly call vaapi of ffmpeg3.2.4 codec module to encode H264 bitstream.
Author:BruceSun
Data:2017.3.8
function:use for test vaapi encoder 
***********************************************************************/

extern "C"
{
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libavutil/hwcontext.h>
}

int main(int argc, char **argv)
{
    if(argc < 5){
        printf("./testvaapi input width height output .(NOTE: only support nv12 format yuv file)\n");
        return -1;
    }
    
    char *input  = argv[1];
    int width    = atoi(argv[2]);
    int height   = atoi(argv[3]);
    char *output = argv[4];
    int ret, got_output;
    int i_frame = 0;
    FILE *file_out, *file_in;
    
    AVCodec *codec = NULL;
    AVCodecContext *codec_ctx= NULL;
    AVFrame *frame_mem_gpu;
    AVFrame *frame_mem;
    AVPacket pkt;
    
    AVBufferRef       *device_ctx_ref;
    AVBufferRef       *hw_device_ctx_ref;
    AVHWDeviceContext *hw_device_ctx;
    
    AVBufferRef       *hw_frames_ctx_ref;
    AVHWFramesContext *hw_frames_ctx;
    
    /* open input .yuv and output .h264 files */
    file_in = fopen(input, "rb");
    if (!file_in) {
        printf("Could not open %s\n", input );
        return -1;
    }
    
    file_out = fopen(output, "wb");
    if (!file_out) {
        printf("Could not open %s\n", output);
        return -1;
    }
    
    /* Register all the codecs */
    avcodec_register_all();
    
    /* Open a device and create an AVHWDeviceContext */
    ret = av_hwdevice_ctx_create(&device_ctx_ref, AV_HWDEVICE_TYPE_VAAPI,
                                    "/dev/dri/renderD128", NULL, 0);
    if (ret < 0) {
        printf("Failed to create a VAAPI device\n");
        return ret;
    }
    
    hw_device_ctx_ref = av_buffer_ref(device_ctx_ref);
    hw_device_ctx = (AVHWDeviceContext*)hw_device_ctx_ref->data;
    
    /* Allocate an AVHWFramesContext tied to a given device context */
    hw_frames_ctx_ref = av_hwframe_ctx_alloc(hw_device_ctx_ref);
    if (!hw_frames_ctx_ref) {
        printf("Failed to create AVHWFramesContext.\n");
        ret = AVERROR(ENOMEM);
        return ret;
    }
    
    /* AVHWFramesContext is filled with the required information */
    hw_frames_ctx = (AVHWFramesContext*)hw_frames_ctx_ref->data;
    hw_frames_ctx->format    = AV_PIX_FMT_VAAPI;
    hw_frames_ctx->sw_format = AV_PIX_FMT_NV12;
    hw_frames_ctx->width     = width;
    hw_frames_ctx->height    = height;
    hw_frames_ctx->initial_pool_size = 0;
    
    /* Finalize AVHWFramesContext before it is attached to any frames. */
    ret = av_hwframe_ctx_init(hw_frames_ctx_ref);
    if (ret < 0) {
        printf("Failed to initialise AVHWFramesContext.\n");
        return ret;
    }
    
    /* find the video encoder */
    codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) {
        printf ("Codec not found.\n");
        return -1;
    }
    
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        printf( "Could not allocate video codec context.\n");
        return -1;
    }
    
    /* Set Encoder Params, I only set some simple params */
    codec_ctx->bit_rate     = 400000;
    codec_ctx->width        = width;
    codec_ctx->height       = height;
    codec_ctx->time_base    = (AVRational){1,25};
    codec_ctx->gop_size     = 10;
    codec_ctx->max_b_frames = 0;
    codec_ctx->pix_fmt      = AV_PIX_FMT_VAAPI;
    
    //For hardware encoders configured to use a hwaccel pixel format.
    //and the AVHWFramesContext describing input frames. 
    //AVHWFramesContext.format must be equal to AVCodecContext.pix_fmt.
    codec_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_ref); 
    
    /* open it */
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        printf ("Could not open codec.\n");
        return -1;
    }
    
    /* Allocate an AVFrame on memory and set the params */
    frame_mem = av_frame_alloc();
    if(!frame_mem){
        printf ("Could not allocate AVFrame\n");
        return -1;
    }
    frame_mem->width  = codec_ctx->width;
    frame_mem->height = codec_ctx->height;
    frame_mem->format = AV_PIX_FMT_NV12;
    ret = av_image_alloc(frame_mem->data, frame_mem->linesize, codec_ctx->width, codec_ctx->height, AV_PIX_FMT_NV12, 16);
    if (ret < 0) {
        printf("Could not allocate AVFrame buffer on memory.\n");
        return -1;
    }
    
    /* Allocate an AVFrame on GPU memory and set the params */
    frame_mem_gpu = av_frame_alloc();
    if (!frame_mem_gpu) {
        printf ("Could not allocate GPU AVFrame.\n");
        return -1;
    }
    frame_mem_gpu->format = codec_ctx->pix_fmt;
    frame_mem_gpu->width  = codec_ctx->width;
    frame_mem_gpu->height = codec_ctx->height;
    ret = av_hwframe_get_buffer( hw_frames_ctx_ref, frame_mem_gpu, 0 );
    if (ret < 0) {
        printf("Could not allocate AVFrame buffer on GPU memory.\n");
        return -1;
    }
    
    /* Encode frames one by one*/
    while(!feof(file_in)){
        fread(frame_mem->data[0], width * height * 1.5, 1, file_in);
        
        frame_mem_gpu->pts = i_frame;
        
        /* Copy data to and from a hw surface.*/
        ret = av_hwframe_transfer_data( frame_mem_gpu, frame_mem, 0 );
        if(ret < 0){
            printf("Copy data to a hw surface failure\n");
            return -1;
        }
        
        /* Packet data will be allocated by the encoder */
        av_init_packet(&pkt);
        pkt.data = NULL;
        pkt.size = 0;
        
        /*Supply a raw video or audio frame to the encoder.*/
        ret = avcodec_send_frame(codec_ctx, frame_mem_gpu);
        if (ret < 0){
            printf("avcodec_send_frame failure\n");
            return -1;
        }
        
        /*Read encoded data from the encoder.*/
        while(1){
            ret = avcodec_receive_packet(codec_ctx, &pkt);
            if (ret == AVERROR(EAGAIN))
                break;
            if (ret < 0)
                exit(1);
            
            printf("Write frame_mem_gpu %3d (size=%5d)\n", i_frame, pkt.size);
            fwrite(pkt.data, 1, pkt.size, file_out);
            av_packet_unref(&pkt);
        }
        i_frame++;
    }
    
    
    /* Close all files and free memory*/
    fclose(file_in);
    fclose(file_out);
    
    avcodec_close(codec_ctx);
    av_free(codec_ctx);
    
    av_frame_free(&frame_mem_gpu);
    av_freep(&frame_mem->data[0]);
    av_frame_free(&frame_mem);
}