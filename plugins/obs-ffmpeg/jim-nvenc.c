#include "jim-nvenc.h"
#include <util/circlebuf.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <obs-avc.h>
#define INITGUID
#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>

/* ========================================================================= */

#define EXTRA_BUFFERS 5

#define error_hr(msg) \
	error("%s: %s: 0x%08lX", __FUNCTION__, msg, (uint32_t)hr);

struct nv_bitstream;
struct nv_texture;

struct handle_tex {
	uint32_t handle;
	ID3D11Texture2D *tex;
	IDXGIKeyedMutex *km;
};

/* ------------------------------------------------------------------------- */
/* Main Implementation Structure                                             */

struct nvenc_data {
	obs_encoder_t *encoder;

	void                     *session;
	NV_ENC_INITIALIZE_PARAMS params;
	NV_ENC_CONFIG            config;
	size_t                   buf_count;
	size_t                   output_delay;
	size_t                   buffers_queued;
	size_t                   next_bitstream;
	size_t                   cur_bitstream;
	bool                     encode_started;
	bool                     first_packet;
	bool                     cbr;
	bool                     bframes;

	DARRAY(struct nv_bitstream) bitstreams;
	DARRAY(struct nv_texture)   textures;
	DARRAY(struct handle_tex)   input_textures;
	struct circlebuf            dts_list;

	DARRAY(uint8_t) packet_data;
	int64_t         packet_pts;
	bool            packet_keyframe;

	ID3D11Device        *device;
	ID3D11DeviceContext *context;

	uint32_t cx;
	uint32_t cy;

	uint8_t *header;
	size_t  header_size;

	uint8_t *sei;
	size_t  sei_size;
};

/* ------------------------------------------------------------------------- */
/* Bitstream Buffer                                                          */

struct nv_bitstream {
	void   *ptr;
	HANDLE event;
};

static bool nv_bitstream_init(struct nvenc_data *enc, struct nv_bitstream *bs)
{
	NV_ENC_CREATE_BITSTREAM_BUFFER buf = {NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
	NV_ENC_EVENT_PARAMS params = {NV_ENC_EVENT_PARAMS_VER};
	HANDLE event = NULL;

	if (NV_FAILED(nv.nvEncCreateBitstreamBuffer(enc->session, &buf))) {
		return false;
	}

	event = CreateEvent(NULL, true, true, NULL);
	if (!event) {
		error("%s: %s", __FUNCTION__, "Failed to create event");
		goto fail;
	}

	params.completionEvent = event;
	if (NV_FAILED(nv.nvEncRegisterAsyncEvent(enc->session, &params))) {
		goto fail;
	}

	bs->ptr = buf.bitstreamBuffer;
	bs->event = event;
	return true;

fail:
	if (event) {
		CloseHandle(event);
	}
	if (buf.bitstreamBuffer) {
		nv.nvEncDestroyBitstreamBuffer(enc->session, buf.bitstreamBuffer);
	}
	return false;
}

static void nv_bitstream_free(struct nvenc_data *enc, struct nv_bitstream *bs)
{
	if (bs->ptr) {
		nv.nvEncDestroyBitstreamBuffer(enc->session, bs->ptr);

		NV_ENC_EVENT_PARAMS params = {NV_ENC_EVENT_PARAMS_VER};
		params.completionEvent = bs->event;
		nv.nvEncUnregisterAsyncEvent(enc->session, &params);
		CloseHandle(bs->event);
	}
}

/* ------------------------------------------------------------------------- */
/* Texture Resource                                                          */

struct nv_texture {
	void            *res;
	ID3D11Texture2D *tex;
	void            *mapped_res;
};

static bool nv_texture_init(struct nvenc_data *enc, struct nv_texture *nvtex)
{
	ID3D11Device *device = enc->device;
	ID3D11Texture2D *tex;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {0};
	desc.Width                = enc->cx;
	desc.Height               = enc->cy;
	desc.MipLevels            = 1;
	desc.ArraySize            = 1;
	desc.Format               = DXGI_FORMAT_NV12;
	desc.SampleDesc.Count     = 1;
	desc.BindFlags            = D3D11_BIND_RENDER_TARGET;

	hr = device->lpVtbl->CreateTexture2D(device, &desc, NULL, &tex);
	if (FAILED(hr)) {
		error_hr("Failed to create texture");
		return false;
	}

	NV_ENC_REGISTER_RESOURCE res = {NV_ENC_REGISTER_RESOURCE_VER};
	res.resourceType             = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
	res.resourceToRegister       = tex;
	res.width                    = enc->cx;
	res.height                   = enc->cy;
	res.bufferFormat             = NV_ENC_BUFFER_FORMAT_NV12;

	if (NV_FAILED(nv.nvEncRegisterResource(enc->session, &res))) {
		tex->lpVtbl->Release(tex);
		return false;
	}

	nvtex->res = res.registeredResource;
	nvtex->tex = tex;
	return true;
}

static void nv_texture_free(struct nvenc_data *enc, struct nv_texture *nvtex)
{
	if (nvtex->res) {
		if (nvtex->mapped_res) {
			nv.nvEncUnmapInputResource(enc->session,
					nvtex->mapped_res);
		}
		nv.nvEncUnregisterResource(enc->session, nvtex->res);
		nvtex->tex->lpVtbl->Release(nvtex->tex);
	}
}

/* ------------------------------------------------------------------------- */
/* Implementation                                                            */

static const char *nvenc_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NvEnc H.264 (Beta)";
}

static inline int nv_get_cap(struct nvenc_data *enc, NV_ENC_CAPS cap)
{
	if (!enc->session)
		return 0;

	NV_ENC_CAPS_PARAM param = {NV_ENC_CAPS_PARAM_VER};
	int v;

	param.capsToQuery = cap;
	nv.nvEncGetEncodeCaps(enc->session, NV_ENC_CODEC_H264_GUID, &param, &v);
	return v;
}

static bool nvenc_update(void *data, obs_data_t *settings)
{
	struct nvenc_data *enc = data;
	if (nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE)) {
		/* Only support reconfiguration of CBR bitrate */
		if (enc->cbr) {
			int bitrate = (int)obs_data_get_int(settings, "bitrate");

			enc->config.rcParams.averageBitRate = bitrate * 1000;
			enc->config.rcParams.maxBitRate     = bitrate * 1000;

			NV_ENC_RECONFIGURE_PARAMS params = {0};
			params.version                   = NV_ENC_RECONFIGURE_PARAMS_VER;
			params.reInitEncodeParams        = enc->params;

			if (FAILED(nv.nvEncReconfigureEncoder(enc->session, &params))) {
				return false;
			}
		}
		return true;
	} else {
		info("This nvidia GPU does not support dynamic bitrate.\n");
	}
	return false;
}

static HANDLE get_lib(const char *lib)
{
	HMODULE mod = GetModuleHandleA(lib);
	if (mod)
		return mod;

	mod = LoadLibraryA(lib);
	if (!mod)
		error("Failed to load %s", lib);
	return mod;
}

typedef HRESULT (WINAPI *CREATEDXGIFACTORY1PROC)(REFIID, void **);

static bool init_d3d11(struct nvenc_data *enc, obs_data_t *settings)
{
	HMODULE                 dxgi  = get_lib("DXGI.dll");
	HMODULE                 d3d11 = get_lib("D3D11.dll");
	CREATEDXGIFACTORY1PROC  create_dxgi;
	PFN_D3D11_CREATE_DEVICE create_device;
	IDXGIFactory1           *factory;
	IDXGIAdapter            *adapter;
	ID3D11Device            *device;
	ID3D11DeviceContext     *context;
	HRESULT                 hr;

	int gpu = (int)obs_data_get_int(settings, "gpu");

	if (!dxgi || !d3d11) {
		return false;
	}

	create_dxgi = (CREATEDXGIFACTORY1PROC)GetProcAddress(dxgi,
			"CreateDXGIFactory1");
	create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11,
			"D3D11CreateDevice");

	if (!create_dxgi || !create_device) {
		error("Failed to load D3D11/DXGI procedures");
		return false;
	}

	hr = create_dxgi(&IID_IDXGIFactory1, &factory);
	if (FAILED(hr)) {
		error_hr("CreateDXGIFactory1 failed");
		return false;
	}

	hr = factory->lpVtbl->EnumAdapters(factory, gpu, &adapter);
	factory->lpVtbl->Release(factory);
	if (FAILED(hr)) {
		error_hr("EnumAdapters failed");
		return false;
	}

	hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
			NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context);
	adapter->lpVtbl->Release(adapter);
	if (FAILED(hr)) {
		error_hr("D3D11CreateDevice failed");
		return false;
	}

	enc->device = device;
	enc->context = context;
	return true;
}

static bool init_session(struct nvenc_data *enc)
{
	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params =
			{NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
	params.device = enc->device;
	params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
	params.apiVersion = NVENCAPI_VERSION;

	if (NV_FAILED(nv.nvEncOpenEncodeSessionEx(&params, &enc->session))) {
		return false;
	}
	return true;
}

static bool init_encoder(struct nvenc_data *enc, obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	const char *level = obs_data_get_string(settings, "level");
	bool temporal_aq = obs_data_get_bool(settings, "temporal_aq");
	bool lookahead = obs_data_get_bool(settings, "la");
	int la_depth = (int)obs_data_get_int(settings, "la_depth");
	bool twopass = obs_data_get_bool(settings, "2pass");
	int gpu = (int)obs_data_get_int(settings, "gpu");
	int bf = (int)obs_data_get_int(settings, "bf");
	NVENCSTATUS err;

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	enc->cx = voi->width;
	enc->cy = voi->height;

	/* -------------------------- */
	/* get preset                 */

	GUID nv_preset = NV_ENC_PRESET_DEFAULT_GUID;
	bool hp = false;
	bool ll = false;

	if (astrcmpi(preset, "hq") == 0) {
		nv_preset = NV_ENC_PRESET_HQ_GUID;

	} else if (astrcmpi(preset, "hp") == 0) {
		nv_preset = NV_ENC_PRESET_HP_GUID;
		hp = true;

	} else if (astrcmpi(preset, "ll") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID;
		ll = true;

	} else if (astrcmpi(preset, "llhq") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
		ll = true;

	} else if (astrcmpi(preset, "llhp") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
		hp = true;
		ll = true;
	}

	if (astrcmpi(rc, "lossless") == 0) {
		nv_preset = hp
			? NV_ENC_PRESET_LOSSLESS_HP_GUID
			: NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID;
	}

	/* -------------------------- */
	/* get preset default config  */

	NV_ENC_PRESET_CONFIG preset_config =
			{NV_ENC_PRESET_CONFIG_VER, {NV_ENC_CONFIG_VER}};

	err = nv.nvEncGetEncodePresetConfig(enc->session,
			NV_ENC_CODEC_H264_GUID, nv_preset, &preset_config);
	if (nv_failed(err, __FUNCTION__, "nvEncGetEncodePresetConfig")) {
		return false;
	}

	/* -------------------------- */
	/* main configuration         */

	enc->config = preset_config.presetCfg;

	uint32_t gop_size = (keyint_sec)
		? keyint_sec * voi->fps_num / voi->fps_den
		: 250;

	NV_ENC_INITIALIZE_PARAMS *params = &enc->params;
	NV_ENC_CONFIG *config = &enc->config;
	NV_ENC_CONFIG_H264 *h264_config = &config->encodeCodecConfig.h264Config;
	NV_ENC_CONFIG_H264_VUI_PARAMETERS *vui_params =
		&h264_config->h264VUIParameters;

	memset(params, 0, sizeof(*params));
	params->version = NV_ENC_INITIALIZE_PARAMS_VER;
	params->encodeGUID = NV_ENC_CODEC_H264_GUID;
	params->presetGUID = nv_preset;
	params->encodeWidth = voi->width;
	params->encodeHeight = voi->height;
	params->darWidth = voi->width;
	params->darHeight = voi->height;
	params->frameRateNum = voi->fps_num;
	params->frameRateDen = voi->fps_den;
	params->enableEncodeAsync = 1;
	params->enablePTD = 1;
	params->encodeConfig = &enc->config;
	params->maxEncodeWidth = voi->width;
	params->maxEncodeHeight = voi->height;
	config->rcParams.averageBitRate = bitrate * 1000;
	config->rcParams.maxBitRate = bitrate * 1000;
	config->gopLength = gop_size;
	config->frameIntervalP = 1 + bf;
	h264_config->idrPeriod = gop_size;
	vui_params->videoSignalTypePresentFlag = 1;
	vui_params->videoFullRangeFlag = (voi->range == VIDEO_RANGE_FULL);
	vui_params->colourDescriptionPresentFlag = 1;
	vui_params->colourMatrix = (voi->colorspace == VIDEO_CS_709) ? 1 : 5;
	vui_params->colourPrimaries = 1;
	vui_params->transferCharacteristics = 1;

	enc->bframes = bf > 0;

	/* lookahead */
	if (!hp && lookahead && nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_LOOKAHEAD)) {
		config->rcParams.lookaheadDepth = (uint16_t)la_depth;
	}

	/* temporal aq */
	if (nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ)) {
		config->rcParams.enableAQ = temporal_aq;
		config->rcParams.enableTemporalAQ = temporal_aq;
	}

	/* -------------------------- */
	/* rate control               */

	enc->cbr = false;

	if (astrcmpi(rc, "cqp") == 0) {
		nv_preset = NV_ENC_PRESET_HQ_GUID;
		config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
		config->rcParams.constQP.qpInterP = cqp;
		config->rcParams.constQP.qpInterB = cqp;
		config->rcParams.constQP.qpIntra = cqp;
		config->rcParams.averageBitRate = 0;
		config->rcParams.maxBitRate = 0;

	} else if (astrcmpi(rc, "lossless") == 0) {
		config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
		config->rcParams.constQP.qpInterP = 0;
		config->rcParams.constQP.qpInterB = 0;
		config->rcParams.constQP.qpIntra = 0;
		config->rcParams.averageBitRate = 0;
		config->rcParams.maxBitRate = 0;

	} else { /* Default to CBR */
		enc->cbr = true;
		h264_config->outputBufferingPeriodSEI = 1;
		h264_config->outputPictureTimingSEI = 1;
		config->rcParams.rateControlMode = twopass
			? NV_ENC_PARAMS_RC_2_PASS_QUALITY
			: NV_ENC_PARAMS_RC_CBR;
	}

	/* -------------------------- */
	/* profile                    */

	if (astrcmpi(profile, "main") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
	} else if (astrcmpi(profile, "baseline") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
	} else {
		config->profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
	}

	/* -------------------------- */
	/* initialize                 */

	if (NV_FAILED(nv.nvEncInitializeEncoder(enc->session, params))) {
		return false;
	}

	enc->buf_count = config->frameIntervalP +
		config->rcParams.lookaheadDepth + EXTRA_BUFFERS;
	enc->output_delay = enc->buf_count - 1;

	info("settings:\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tpreset:       %s\n"
	     "\tprofile:      %s\n"
	     "\tlevel:        %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n"
	     "\t2-pass:       %s\n"
	     "\tb-frames:     %d\n"
	     "\tGPU:          %d\n",
	     rc, (int)config->rcParams.maxBitRate, cqp, gop_size,
	     preset, profile, level,
	     enc->cx, enc->cy,
	     twopass ? "true" : "false",
	     bf, gpu);

	return true;
}

static bool init_bitstreams(struct nvenc_data *enc)
{
	da_reserve(enc->bitstreams, enc->buf_count);
	for (size_t i = 0; i < enc->buf_count; i++) {
		struct nv_bitstream bitstream;
		if (!nv_bitstream_init(enc, &bitstream)) {
			return false;
		}

		da_push_back(enc->bitstreams, &bitstream);
	}

	return true;
}

static bool init_textures(struct nvenc_data *enc)
{
	da_reserve(enc->bitstreams, enc->buf_count);
	for (size_t i = 0; i < enc->buf_count; i++) {
		struct nv_texture texture;
		if (!nv_texture_init(enc, &texture)) {
			return false;
		}

		da_push_back(enc->textures, &texture);
	}

	return true;
}

static void nvenc_destroy(void *data);

static void *nvenc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	NV_ENCODE_API_FUNCTION_LIST init = {NV_ENCODE_API_FUNCTION_LIST_VER};
	struct nvenc_data *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->first_packet = true;

	if (!obs_nv12_tex_active()) {
		goto fail;
	}
	if (!init_nvenc()) {
		goto fail;
	}
	if (NV_FAILED(nv_create_instance(&init))) {
		goto fail;
	}
	if (!init_d3d11(enc, settings)) {
		goto fail;
	}
	if (!init_session(enc)) {
		goto fail;
	}
	if (!init_encoder(enc, settings)) {
		goto fail;
	}
	if (!init_bitstreams(enc)) {
		goto fail;
	}
	if (!init_textures(enc)) {
		goto fail;
	}

	return enc;

fail:
	nvenc_destroy(enc);
	return obs_encoder_create_rerouted(encoder, "actual_ffmpeg_nvenc");
}

static bool get_encoded_packet(struct nvenc_data *enc, bool finalize);

static void nvenc_destroy(void *data)
{
	struct nvenc_data *enc = data;

	for (size_t i = 0; i < enc->textures.num; i++) {
		nv_texture_free(enc, &enc->textures.array[i]);
	}
	if (enc->encode_started) {
		size_t next_bitstream = enc->next_bitstream;
		HANDLE next_event = enc->bitstreams.array[next_bitstream].event;

		NV_ENC_PIC_PARAMS params = {NV_ENC_PIC_PARAMS_VER};
		params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
		params.completionEvent = next_event;
		nv.nvEncEncodePicture(enc->session, &params);
		get_encoded_packet(enc, true);
	}
	for (size_t i = 0; i < enc->bitstreams.num; i++) {
		nv_bitstream_free(enc, &enc->bitstreams.array[i]);
	}
	if (enc->session) {
		nv.nvEncDestroyEncoder(enc->session);
	}
	for (size_t i = 0; i < enc->input_textures.num; i++) {
		ID3D11Texture2D *tex = enc->input_textures.array[i].tex;
		IDXGIKeyedMutex *km  = enc->input_textures.array[i].km;
		tex->lpVtbl->Release(tex);
		km->lpVtbl->Release(km);
	}
	if (enc->context) {
		enc->context->lpVtbl->Release(enc->context);
	}
	if (enc->device) {
		enc->device->lpVtbl->Release(enc->device);
	}

	bfree(enc->header);
	bfree(enc->sei);
	circlebuf_free(&enc->dts_list);
	da_free(enc->textures);
	da_free(enc->bitstreams);
	da_free(enc->input_textures);
	da_free(enc->packet_data);
	bfree(enc);
}

static ID3D11Texture2D *get_tex_from_handle(struct nvenc_data *enc,
		uint32_t handle, IDXGIKeyedMutex **km_out)
{
	ID3D11Device    *device = enc->device;
	ID3D11Texture2D *input_tex;
	IDXGIKeyedMutex *km;
	HRESULT         hr;

	for (size_t i = 0; i < enc->input_textures.num; i++) {
		struct handle_tex *ht = &enc->input_textures.array[i];
		if (ht->handle == handle) {
			*km_out = ht->km;
			return ht->tex;
		}
	}

	hr = device->lpVtbl->OpenSharedResource(device,
			(HANDLE)(uintptr_t)handle,
			&IID_ID3D11Texture2D, &input_tex);
	if (FAILED(hr)) {
		error_hr("OpenSharedResource failed");
		return NULL;
	}

	hr = input_tex->lpVtbl->QueryInterface(input_tex, &IID_IDXGIKeyedMutex,
			&km);
	if (FAILED(hr)) {
		error_hr("QueryInterface(IDXGIKeyedMutex) failed");
		input_tex->lpVtbl->Release(input_tex);
		return NULL;
	}

	input_tex->lpVtbl->SetEvictionPriority(input_tex,
			DXGI_RESOURCE_PRIORITY_MAXIMUM);

	*km_out = km;

	struct handle_tex new_ht = {handle, input_tex, km};
	da_push_back(enc->input_textures, &new_ht);
	return input_tex;
}

static bool get_encoded_packet(struct nvenc_data *enc, bool finalize)
{
	void *s = enc->session;

	da_resize(enc->packet_data, 0);

	if (!enc->buffers_queued)
		return true;
	if (!finalize && enc->buffers_queued < enc->output_delay)
		return true;

	size_t count = finalize ? enc->buffers_queued : 1;

	for (size_t i = 0; i < count; i++) {
		size_t cur_bs_idx          = enc->cur_bitstream;
		struct nv_bitstream *bs    = &enc->bitstreams.array[cur_bs_idx];
		struct nv_texture   *nvtex = &enc->textures.array[cur_bs_idx];

		/* ---------------- */

		NV_ENC_LOCK_BITSTREAM lock = {NV_ENC_LOCK_BITSTREAM_VER};
		lock.outputBitstream       = bs->ptr;
		lock.doNotWait             = false;

		if (NV_FAILED(nv.nvEncLockBitstream(s, &lock))) {
			return false;
		}

		if (enc->first_packet) {
			uint8_t *new_packet;
			size_t size;

			enc->first_packet = false;
			obs_extract_avc_headers(
					lock.bitstreamBufferPtr,
					lock.bitstreamSizeInBytes,
					&new_packet, &size,
					&enc->header, &enc->header_size,
					&enc->sei, &enc->sei_size);

			da_copy_array(enc->packet_data, new_packet, size);
			bfree(new_packet);
		} else {
			da_copy_array(enc->packet_data,
					lock.bitstreamBufferPtr,
					lock.bitstreamSizeInBytes);
		}

		enc->packet_pts = (int64_t)lock.outputTimeStamp;
		enc->packet_keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;

		if (NV_FAILED(nv.nvEncUnlockBitstream(s, bs->ptr))) {
			return false;
		}

		/* ---------------- */

		if (nvtex->mapped_res) {
			NVENCSTATUS err;
			err = nv.nvEncUnmapInputResource(s, nvtex->mapped_res);
			if (nv_failed(err, __FUNCTION__, "unmap")) {
				return false;
			}
			nvtex->mapped_res = NULL;
		}

		/* ---------------- */

		if (++enc->cur_bitstream == enc->buf_count)
			enc->cur_bitstream = 0;

		enc->buffers_queued--;
	}

	return true;
}

static bool nvenc_encode_tex(void *data, gs_texture_t *tex,
		uint64_t lock_key, int64_t pts,
		struct encoder_packet *packet, bool *received_packet)
{
	struct nvenc_data   *enc     = data;
	uint32_t            handle   = gs_texture_get_shared_handle(tex);
	ID3D11Device        *device  = enc->device;
	ID3D11DeviceContext *context = enc->context;
	ID3D11Texture2D     *input_tex;
	ID3D11Texture2D     *output_tex;
	struct nv_texture   *nvtex;
	IDXGIKeyedMutex     *km;
	struct nv_bitstream *bs;
	NVENCSTATUS         err;

	if (handle == GS_INVALID_HANDLE) {
		error("Encode failed: bad texture handle");
		return false;
	}

	circlebuf_push_back(&enc->dts_list, &pts, sizeof(pts));

	bs    = &enc->bitstreams.array[enc->next_bitstream];
	nvtex = &enc->textures.array[enc->next_bitstream];

	input_tex  = get_tex_from_handle(enc, handle, &km);
	output_tex = nvtex->tex;

	if (!input_tex) {
		return false;
	}

	/* ------------------------------------ */
	/* wait for output bitstream/tex        */

	WaitForSingleObject(bs->event, INFINITE);

	/* ------------------------------------ */
	/* copy to output tex                   */

	km->lpVtbl->AcquireSync(km, lock_key, INFINITE);

	context->lpVtbl->CopyResource(context,
			(ID3D11Resource *)output_tex,
			(ID3D11Resource *)input_tex);

	km->lpVtbl->ReleaseSync(km, 0);

	/* ------------------------------------ */
	/* map output tex so nvenc can use it   */

	NV_ENC_MAP_INPUT_RESOURCE map = {NV_ENC_MAP_INPUT_RESOURCE_VER};
	map.registeredResource        = nvtex->res;
	if (NV_FAILED(nv.nvEncMapInputResource(enc->session, &map))) {
		return false;
	}

	nvtex->mapped_res = map.mappedResource;

	/* ------------------------------------ */
	/* do actual encode call                */

	NV_ENC_PIC_PARAMS params = {0};
	params.version           = NV_ENC_PIC_PARAMS_VER;
	params.pictureStruct     = NV_ENC_PIC_STRUCT_FRAME;
	params.inputBuffer       = nvtex->mapped_res;
	params.bufferFmt         = NV_ENC_BUFFER_FORMAT_NV12;
	params.inputTimeStamp    = (uint64_t)pts;
	params.inputWidth        = enc->cx;
	params.inputHeight       = enc->cy;
	params.outputBitstream   = bs->ptr;
	params.completionEvent   = bs->event;

	err = nv.nvEncEncodePicture(enc->session, &params);
	if (err != NV_ENC_SUCCESS && err != NV_ENC_ERR_NEED_MORE_INPUT) {
		nv_failed(err, __FUNCTION__, "nvEncEncodePicture");
		return false;
	}

	enc->encode_started = true;
	enc->buffers_queued++;

	if (++enc->next_bitstream == enc->buf_count) {
		enc->next_bitstream = 0;
	}

	/* ------------------------------------ */
	/* check for encoded packet and parse   */

	if (!get_encoded_packet(enc, false)) {
		return false;
	}

	/* ------------------------------------ */
	/* output encoded packet                */

	if (enc->packet_data.num) {
		int64_t dts;
		circlebuf_pop_front(&enc->dts_list, &dts, sizeof(dts));

		/* subtract bframe delay from dts */
		if (enc->bframes)
			dts--;

		*received_packet = true;
		packet->data     = enc->packet_data.array;
		packet->size     = enc->packet_data.num;
		packet->type     = OBS_ENCODER_VIDEO;
		packet->pts      = enc->packet_pts;
		packet->dts      = dts;
		packet->keyframe = enc->packet_keyframe;
	} else {
		*received_packet = false;
	}

	return true;
}

void nvenc_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_int(settings, "cqp", 23);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_string(settings, "preset", "default");
	obs_data_set_default_string(settings, "profile", "main");
	obs_data_set_default_string(settings, "level", "auto");
	obs_data_set_default_bool(settings, "2pass", true);
	obs_data_set_default_bool(settings, "temporal_aq", true);
	obs_data_set_default_int(settings, "gpu", 0);
	obs_data_set_default_int(settings, "bf", 2);
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p,
		obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cqp = astrcmpi(rc, "CQP") == 0;
	bool lossless = astrcmpi(rc, "lossless") == 0;
	size_t count;

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !cqp && !lossless);
	p = obs_properties_get(ppts, "cqp");
	obs_property_set_visible(p, cqp);

	p = obs_properties_get(ppts, "preset");
	count = obs_property_list_item_count(p);

	for (size_t i = 0; i < count; i++) {
		bool compatible = (i == 0 || i == 2);
		obs_property_list_item_disable(p, i, lossless && !compatible);
	}

	return true;
}

obs_properties_t *nvenc_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "rate_control",
			obs_module_text("RateControl"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "CBR", "CBR");
	obs_property_list_add_string(p, "VBR", "VBR");
	obs_property_list_add_string(p, "CQP", "CQP");
	obs_property_list_add_string(p, obs_module_text("Lossless"),
			"lossless");

	obs_property_set_modified_callback(p, rate_control_modified);

	obs_properties_add_int(props, "bitrate",
			obs_module_text("Bitrate"), 50, 300000, 50);

	obs_properties_add_int(props, "cqp", "CQP", 0, 50, 1);

	obs_properties_add_int(props, "keyint_sec",
			obs_module_text("KeyframeIntervalSec"), 0, 10, 1);

	p = obs_properties_add_list(props, "preset", obs_module_text("Preset"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

#define add_preset(val) \
	obs_property_list_add_string(p, obs_module_text("NVENC.Preset." val), \
			val)
	add_preset("default");
	add_preset("hq");
	add_preset("hp");
	add_preset("bd");
	add_preset("ll");
	add_preset("llhq");
	add_preset("llhp");
#undef add_preset

	p = obs_properties_add_list(props, "profile", obs_module_text("Profile"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

#define add_profile(val) \
	obs_property_list_add_string(p, val, val)
	add_profile("high");
	add_profile("main");
	add_profile("baseline");
	add_profile("high444p");

	p = obs_properties_add_list(props, "level",
			obs_module_text("NVENC.Level"), OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_STRING);
	add_profile("auto");
	add_profile("1"   );
	add_profile("1.0" );
	add_profile("1b"  );
	add_profile("1.0b");
	add_profile("1.1" );
	add_profile("1.2" );
	add_profile("1.3" );
	add_profile("2"   );
	add_profile("2.0" );
	add_profile("2.1" );
	add_profile("2.2" );
	add_profile("3"   );
	add_profile("3.0" );
	add_profile("3.1" );
	add_profile("3.2" );
	add_profile("4"   );
	add_profile("4.0" );
	add_profile("4.1" );
	add_profile("4.2" );
	add_profile("5"   );
	add_profile("5.0" );
	add_profile("5.1" );
#undef add_profile

	obs_properties_add_bool(props, "2pass",
			obs_module_text("NVENC.Use2Pass"));
	obs_properties_add_bool(props, "temporal_aq",
			obs_module_text("NVENC.TemporalAQ"));
	obs_properties_add_int(props, "gpu", obs_module_text("GPU"), 0, 8, 1);

	obs_properties_add_int(props, "bf", obs_module_text("BFrames"),
			0, 4, 1);

	return props;
}

static bool nvenc_extra_data(void *data, uint8_t **header, size_t *size)
{
	struct nvenc_data *enc = data;

	if (!enc->header) {
		return false;
	}

	*header = enc->header;
	*size   = enc->header_size;
	return true;
}

static bool nvenc_sei_data(void *data, uint8_t **sei, size_t *size)
{
	struct nvenc_data *enc = data;

	if (!enc->sei) {
		return false;
	}

	*sei  = enc->sei;
	*size = enc->header_size;
	return true;
}

struct obs_encoder_info nvenc_info = {
	.id                      = "ffmpeg_nvenc",
	.codec                   = "h264",
	.type                    = OBS_ENCODER_VIDEO,
	.caps                    = OBS_ENCODER_CAP_PASS_TEXTURE,
	.get_name                = nvenc_get_name,
	.create                  = nvenc_create,
	.destroy                 = nvenc_destroy,
	.update                  = nvenc_update,
	.encode_texture          = nvenc_encode_tex,
	.get_defaults            = nvenc_defaults,
	.get_properties          = nvenc_properties,
	.get_extra_data          = nvenc_extra_data,
	.get_sei_data            = nvenc_sei_data,
};
