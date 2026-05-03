#ifndef _DEFINE_H
#define _DEFINE_H

#define DRIVER_NAME "MyDriver4"
#define MEM_TAG '10hZ' // 内存标签

#define MSG_CODE_NORMAL_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#define MSG_CODE_NORMAL_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)
#define MSG_CODE_REQUEST_APP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_OUT_DIRECT, FILE_READ_ACCESS) // 驱动 -> 应用 请求APP
#define MSG_CODE_CMD_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_IN_DIRECT, FILE_WRITE_ACCESS) // 应用 -> 驱动 cmd任务

#define MAX_NORMAL_BUFFER_SIZE 1048576 // 正常最大缓冲区大小: 1MB
#define MIN_REQUEST_APP_BUFFER_SIZE 65536 // 请求APP读最小缓冲区大小: 64KB (保证传输性能)
#define MAX_CMD_CONCURR_NUM 4096 // 允许同时存在cmd任务的数量

/*
 * SUFFICIENT_REQUEST_APP_IRP_NUM 首次运行时, 应用程序必须同时投放高于此值的requestApp的数量, 否则驱动程序将不响应其他IRP请求(挂起状态)
 * WARNING_REQUEST_APP_IRP_NUM 随着requestApp, 为保证系统效率, 若requestApp队列数量低于此值, 系统效率会降低, 会消耗一个requestApp irp来警告应用层, 加大requestApp投放量
 * 若requestApp全部消耗完, 驱动程序会进入挂起状态
 */
#define SUFFICIENT_REQUEST_APP_IRP_NUM 1024 // 充分同时存在requestApp数量
#define WARNING_REQUEST_APP_IRP_NUM 64 // 同时存在requestApp数量

#define CANCEL_STAT_DO_NOTHING 0
#define CANCEL_STAT_SUCCESS 1
#define CANCEL_STAT_CANCELLED_BY_SELF 2
#define CANCEL_STAT_CANCELLED_BY_ROUTINE 3
#define IS_CANCELLED(stat) (stat == CANCEL_STAT_CANCELLED_BY_SELF || stat == CANCEL_STAT_CANCELLED_BY_ROUTINE)

typedef struct _MY_IRP_NODE {
	LIST_ENTRY entry;
	PIRP irp;
	UCHAR fnType; // 派遣类型
	char* outBuffer; // 读缓冲区
	ULONG outSize; // 读请求的缓冲区大小
	char* inBuffer; // 写缓冲区
	ULONG inSize; // 写请求的缓冲区大小
	ULONG completedSize; // 已完成传输的数据长度, 仅写操作用到该选项
} MY_IRP_NODE, * PMY_IRP_NODE;

typedef struct _MY_CMD_NODE {
	LIST_ENTRY entry;
	char* buffer; // 缓冲区
	ULONG bufferSize; // 缓冲区大小
	DWORD32 cmdID; // 任务号
} MY_CMD_NODE, *PMY_CMD_NODE;

#endif // !_DEFINE_H
