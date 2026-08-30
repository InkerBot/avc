

#ifndef AVC_PLUGIN_H
#define AVC_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVC_ABI_VERSION 2u

#if defined(_WIN32)
#define AVC_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define AVC_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define AVC_PLUGIN_EXPORT
#endif

typedef enum AvcParamType {
    AVC_PARAM_FLOAT = 0,
    AVC_PARAM_ENUM = 1,
    AVC_PARAM_BOOL = 2,
    AVC_PARAM_TEXT = 3,
    AVC_PARAM_PATH = 4
} AvcParamType;

typedef enum AvcParamCurve {
    AVC_CURVE_LINEAR = 0,
    AVC_CURVE_LOG = 1
} AvcParamCurve;

typedef struct AvcParamDesc {
    const char *name; 
    AvcParamType type;
    float min;
    float max;
    float default_value;
    const char *unit;        
    AvcParamCurve curve;
    const char *const *values; 
    const char *description;   
    const char *default_text;  
} AvcParamDesc;

typedef enum AvcPortTransport {
    AVC_PORT_STREAM = 0,
    AVC_PORT_VALUE = 1
} AvcPortTransport;

typedef struct AvcPortTypeDesc {
    const char *name;  
    const char *label; 
    AvcPortTransport transport;

    
    uint32_t bytes;
} AvcPortTypeDesc;

typedef struct AvcPortDesc {
    const char *name;

    
    const char *type;
} AvcPortDesc;

typedef struct AvcNodeDesc {
    
    const char *type;
    const char *category; 
    const char *label;    

    const AvcPortDesc *inputs;
    uint32_t input_count;
    const AvcPortDesc *outputs;
    uint32_t output_count;

    const AvcParamDesc *params;
    uint32_t param_count;

    
    uint32_t dynamic_inputs;
    uint32_t dynamic_outputs;

    
    uint32_t latency_frames;

    
    uint32_t realtime_safe;

    
    uint32_t recommended_cold_block;
} AvcNodeDesc;

typedef struct AvcPrepareInfo {
    uint32_t sample_rate;
    uint32_t max_quantum; 
    uint32_t n_inputs;
    uint32_t n_outputs;
} AvcPrepareInfo;

typedef enum AvcResult {
    AVC_RESULT_OK = 0,
    AVC_RESULT_ERROR = 1
} AvcResult;

typedef enum AvcNodeState {
    AVC_NODE_OFFLINE = 0,
    AVC_NODE_LOADING = 1,
    AVC_NODE_READY = 2,
    AVC_NODE_DEGRADED = 3,
    AVC_NODE_ERROR = 4
} AvcNodeState;

typedef struct AvcNodeStatus {
    uint32_t struct_size; 
    AvcNodeState state;
    float progress; 
    uint64_t processed_blocks;
    uint64_t bypassed_blocks;
    uint64_t failures;
    char message[256];
} AvcNodeStatus;

typedef struct AvcProcessCtx {
    
    const float *const *inputs;
    float *const *outputs;
    uint32_t n_inputs;
    uint32_t n_outputs;
    uint32_t nframes;

    
    const void *const *in_blocks;
    void *const *out_blocks;

    
    uint64_t start_frame;
    uint32_t discontinuity;
} AvcProcessCtx;

typedef struct AvcNodeVtable {
    void *(*create)(void);
    void (*destroy)(void *self);

    
    AvcResult (*prepare)(void *self, const AvcPrepareInfo *info, char *error,
                         uint32_t error_capacity);

    
    void (*inherit)(void *self, const void *previous);

    
    uint32_t (*latency_frames)(const void *self);

    void (*set_param)(void *self, uint32_t index, float value);
    void (*set_option)(void *self, uint32_t index, const char *value);
    void (*process)(void *self, const AvcProcessCtx *ctx);

    
    AvcResult (*get_status)(const void *self, AvcNodeStatus *status);
} AvcNodeVtable;

typedef struct AvcNodeType {
    AvcNodeDesc desc;
    AvcNodeVtable vtable;
} AvcNodeType;

typedef enum AvcSettingType {
    AVC_SETTING_BOOL = 0,
    AVC_SETTING_INT = 1,
    AVC_SETTING_FLOAT = 2,
    AVC_SETTING_TEXT = 3,
    AVC_SETTING_ENUM = 4,
    AVC_SETTING_PATH = 5
} AvcSettingType;

typedef struct AvcSettingDesc {
    const char *key;
    const char *label;       
    const char *description; 
    AvcSettingType type;
    double min; 
    double max;
    
    const char *default_value;
    const char *const *values; 
} AvcSettingDesc;

typedef struct AvcPresetDesc {
    const char *name;
    const char *json;
} AvcPresetDesc;

typedef struct AvcUiAssetDesc {
    uint32_t struct_size; 
    const char *path;
    const char *mime_type;
    const void *data;
    uint32_t data_size;
} AvcUiAssetDesc;

typedef struct AvcUiRequest {
    uint32_t struct_size; 
    uint64_t request_id;
    const char *method;
    const char *json;
    uint32_t json_size;
} AvcUiRequest;

typedef struct AvcPlugin {
    uint32_t abi_version; 
    uint32_t struct_size; 

    
    const char *id;

    const char *name;        
    const char *version;     
    const char *author;      
    const char *description; 

    const AvcNodeType *nodes;
    uint32_t node_count;

    
    const AvcPortTypeDesc *port_types;
    uint32_t port_type_count;

    const AvcSettingDesc *settings;
    uint32_t setting_count;

    const AvcPresetDesc *presets;
    uint32_t preset_count;

    
    const AvcUiAssetDesc *ui_assets;
    uint32_t ui_asset_count;
    const char *ui_entry;

    
    void (*handle_ui_request)(const AvcUiRequest *request);

    
    void (*configure)(const char *key, const char *value);

    
    void (*shutdown)(void);
} AvcPlugin;

typedef enum AvcLogLevel {
    AVC_LOG_TRACE = 0,
    AVC_LOG_DEBUG = 1,
    AVC_LOG_INFO = 2,
    AVC_LOG_WARN = 3,
    AVC_LOG_ERROR = 4
} AvcLogLevel;

typedef struct AvcHostApi {
    uint32_t abi_version; 
    uint32_t struct_size; 
    void *context;        

    
    void (*log)(void *context, AvcLogLevel level, const char *message);

    
    const char *(*setting)(void *context, const char *key);

    
    const char *config_dir;

    
    const char *data_dir;

    
    void (*ui_reply)(void *context, uint64_t request_id, AvcResult result,
                     const char *json, uint32_t json_size, const char *error);

    
    void (*ui_emit)(void *context, const char *event, const char *json,
                    uint32_t json_size);
} AvcHostApi;

AVC_PLUGIN_EXPORT const AvcPlugin *avc_plugin_init(const AvcHostApi *host);

#ifdef __cplusplus
} 
#endif

#endif 
