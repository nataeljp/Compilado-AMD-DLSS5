// ABI C da LmxxfNrRuntime.dll (o runtime da rede DLSS 5 do lmxxf, MIT, com o adaptador do projeto), so o que o
// overlay usa. Layout binario fixo: nao reordenar campos.
#pragma once
#include <stdint.h>

extern "C" {

#define LMXXF_NR_ABI_VERSION 1u

enum LmxxfNrStatus
{
    LMXXF_NR_OK = 0,
    LMXXF_NR_UNSUPPORTED_ABI = 1,
    LMXXF_NR_INVALID_ARGUMENT = 2,
    LMXXF_NR_NOT_IMPLEMENTED = 3,
    LMXXF_NR_UNAVAILABLE = 4,
    LMXXF_NR_FAILED = 5
};

struct LmxxfNrCapabilities
{
    uint32_t struct_size, abi_version, max_input_width, max_input_height;
    uint32_t history_supported, overlap_supported, graph_supported, hip_ready, gfx1201_target;
};

struct LmxxfNrCreateInfo
{
    uint32_t struct_size;
    void* device;                     // ID3D12Device*
    void* queue;                      // ID3D12CommandQueue*
    const wchar_t* assets_directory;  // pasta native-game-tiled-assets
    uint32_t flags;                   // 0
};

// bandeiras do quadro
#define LMXXF_NR_FRAME_FLAG_STRENGTH (1u << 0)     // forcas de detalhe/cor da decodificacao
#define LMXXF_NR_FRAME_FLAG_TEMPORAL (1u << 3)     // historico por movimento (o adaptador atual ignora)
#define LMXXF_NR_FRAME_FLAG_FULL_NETWORK (1u << 5) // os 71 blocos
#define LMXXF_NR_FRAME_FLAG_CONTROLS (1u << 6)     // controles tom/estrutura/pele/outro/estilo

struct LmxxfNrFrameInfo // 144 bytes
{
    uint32_t struct_size;
    uint64_t session_id, frame_id, list_generation;
    void* command_list; // ID3D12GraphicsCommandList*
    uint32_t color_width, color_height;
    void* color;          // ID3D12Resource*
    uint32_t color_state; // D3D12_RESOURCE_STATES
    uint32_t flags;
    float transfer_strength, color_strength; // 0..1
    uint32_t debug_view;
    float model_scale;
    void* motion; // RG float, deslocamento em pixels ate o quadro anterior
    uint32_t motion_state, motion_width, motion_height;
    float motion_scale_x, motion_scale_y;
    uint32_t history_reset;
    uint32_t passes; // 1..3
    float output_smooth, output_smooth_threshold;
    float control_tone, control_structure, control_skin, control_other, control_style; // -1..4
};

struct LmxxfNrJob
{
    uint32_t struct_size;
    void* handle;
    void* private_output; // ID3D12Resource* com a resposta, depois de RecordOutputs
};

struct LmxxfNrApi
{
    uint32_t struct_size, abi_version;
    int32_t (*QueryCapabilities)(LmxxfNrCapabilities*);
    int32_t (*Create)(const LmxxfNrCreateInfo*, void** context);
    int32_t (*Destroy)(void*);
    int32_t (*PrepareSession)(void*);
    int32_t (*PrepareFrame)(void*, const LmxxfNrFrameInfo*, LmxxfNrJob*);
    int32_t (*RecordInputs)(void*, void* job, void* command_list);
    int32_t (*EnqueueHip)(void*, void* job, void* command_queue);
    int32_t (*RecordOutputs)(void*, void* job, void* command_list);
    int32_t (*ExecuteAfterProducer)(void*, void* job, void* command_queue);
    int32_t (*CancelUnsubmitted)(void*, void* job);
    int32_t (*Poll)(void*, void* job, uint32_t* state);
    int32_t (*Retire)(void*, void* job);
    int32_t (*ResetHistory)(void*);
    int32_t (*Drain)(void*);
    int32_t (*GetStatus)(void*, char* buf, uint32_t chars);
    int32_t (*GetLastError)(char* buf, uint32_t chars);
    int32_t (*EnqueueHipAsync)(void*, void* job, void* command_queue);
    int32_t (*AbandonJob)(void*, void* job);
    int32_t (*OutputReady)(void*, void* job, uint32_t* ready);
};

} // extern "C"

static_assert(sizeof(LmxxfNrFrameInfo) == 144, "LmxxfNrFrameInfo: layout da ABI");
