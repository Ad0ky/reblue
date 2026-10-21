/*
 * re:Blue: MPEG-1 video decoder for Blue Dragon's Sofdec (.sfd) cutscenes.
 *
 * The game's movies are MPEG-1 video (1280x720, 29.97 fps, with B pictures) in
 * an MPEG program stream. mpeg1video_decoder_select in configure is "mpegvideo",
 * which is already on for the WMV3 decoder (config_nfsmw_wmv3.h) along with
 * what it selects, so only the decoder itself is enabled here.
 * config.h includes this after config_nfsmw_wmv3.h.
 */
#undef CONFIG_MPEG1VIDEO_DECODER
#define CONFIG_MPEG1VIDEO_DECODER 1
