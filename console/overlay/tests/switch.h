#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef int32_t Result;
typedef struct {int active;} Service;typedef Service TipcService;
typedef struct {void *impl;} Thread;
typedef enum { NsApplicationControlSource_StorageOnly=2 } NsApplicationControlSource;
typedef struct {
    struct { char display_version[16]; u64 local_communication_id[8]; } nacp;
} NsApplicationControlData;
typedef struct {u64 value;} SmServiceName;
typedef enum {SetLanguage_ENUS=1,SetLanguage_ZHCN=6,SetLanguage_ZHTW=11,SetLanguage_ZHHANS=15,SetLanguage_ZHHANT=16} SetLanguage;
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
#define MAKERESULT(m,d) ((m)*512+(d))
#define Module_Libnx 345
#define LibnxError_BadInput 2
#define HidNpadButton_A        (UINT64_C(1) << 0)
#define HidNpadButton_B        (UINT64_C(1) << 2)
#define HidNpadButton_Y        (UINT64_C(1) << 3)
#define HidNpadButton_L        (UINT64_C(1) << 4)
#define HidNpadButton_R        (UINT64_C(1) << 5)
#define HidNpadButton_Plus     (UINT64_C(1) << 6)
#define HidNpadButton_Minus    (UINT64_C(1) << 7)
#define HidNpadButton_AnyLeft  (UINT64_C(1) << 8)
#define HidNpadButton_AnyRight (UINT64_C(1) << 9)
#define HidNpadButton_X        (UINT64_C(1) << 10)
typedef struct { u32 x, y; } HidTouchState;
typedef struct { int x, y; } HidAnalogStickState;
typedef struct {int out_num_objects;Service*out_objects; int buffer_attrs[8]; struct {void *ptr; size_t size;} buffers[8];} MockParams;
#define SfBufferAttr_Out 1
#define SfBufferAttr_HipcMapAlias 2
#define SfBufferAttr_In 4
#ifdef __cplusplus
extern "C" {
#endif
Result mockDispatch(Service*,u32,const void*,size_t,void*,size_t,MockParams);
bool serviceIsActive(Service*);void serviceClose(Service*);
Result smGetService(Service*,const char*);SmServiceName smEncodeName(const char*);TipcService*smGetServiceSessionTipc(void);
Result smInitialize(void);void smExit(void);
Result threadCreate(Thread*,void (*)(void*),void*,void*,size_t,int,int);
Result threadStart(Thread*);Result threadWaitForExit(Thread*);Result threadClose(Thread*);
Result nsInitialize(void);void nsExit(void);
Result nsIsAnyApplicationEntityInstalled(u64,bool*);
Result nsGetApplicationControlData(NsApplicationControlSource,u64,NsApplicationControlData*,size_t,u64*);
void svcSleepThread(int64_t);
Result setInitialize(void);void setExit(void);Result setGetSystemLanguage(u64*);Result setMakeLanguage(u64,SetLanguage*);
u64 armGetSystemTick(void);u64 armGetSystemTickFreq(void);
#ifdef __cplusplus
}
#endif
#define serviceDispatch(s,c,...) mockDispatch((s),(c),NULL,0,NULL,0,(MockParams){__VA_ARGS__})
#define serviceDispatchOut(s,c,o,...) mockDispatch((s),(c),NULL,0,&(o),sizeof(o),(MockParams){__VA_ARGS__})
#define serviceDispatchIn(s,c,i,...) mockDispatch((s),(c),&(i),sizeof(i),NULL,0,(MockParams){__VA_ARGS__})
#define serviceDispatchInOut(s,c,i,o,...) mockDispatch((s),(c),&(i),sizeof(i),&(o),sizeof(o),(MockParams){__VA_ARGS__})
#define tipcDispatchInOut(s,c,i,o) mockDispatch((s),(c),&(i),sizeof(i),&(o),sizeof(o),(MockParams){0})
