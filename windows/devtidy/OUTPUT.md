# devtidy stdout 字段说明

`devtidy` 的标准输出为 `JSONL`，即每一行都是一个独立的 JSON 对象，便于上层进程逐行读取和解析。

## 顶层结构

每一行都固定为以下结构：

```json
{
  "schema": "v1",
  "level": "info|error",
  "event": "snake_case_event_name",
  "device": "YumeDiskSCSI|YumeDiskKMDF|null",
  "data": {}
}
```

顶层字段说明：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schema` | string | 输出版本，当前固定为 `v1` |
| `level` | string | 事件级别，当前为 `info` 或 `error` |
| `event` | string | 事件名，使用 `snake_case` |
| `device` | string\|null | 事件所属设备；全局事件为 `null` |
| `data` | object | 事件负载；具体字段由 `event` 决定 |

## 通用字段约定

`data` 中常见字段说明：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `package_root` | string | 驱动包根目录，一般为 `devtidy.exe` 所在目录 |
| `package_dir` | string | 某个设备对应的包目录，例如 `...\\YumeDiskSCSI` |
| `source_inf` | string | 包目录中的源 `.inf` 路径 |
| `published_inf` | string | 被系统成功接纳后的发布 `.inf` 名称或路径 |
| `files` | array<string> | 扫描到的多个 `.inf` 文件名 |
| `instance_id` | string | 设备实例 ID |
| `instance_count` | uint | 当前检测到的同硬件 ID 设备总数 |
| `duplicate_count` | uint | 重复设备数量，等于 `instance_count - 1` |
| `need_reboot` | bool | 本次设备删除或驱动包删除后系统是否要求重启 |
| `error_code` | uint | Win32 错误码 |
| `error` | string | 错误描述 |
| `hint` | string | 非致命提示或操作建议 |
| `syntax` | string | 命令行用法 |
| `ok` | bool | 汇总结果；`true` 表示本次执行整体成功 |

## 事件说明

### 全局事件

| `event` | `level` | `data` 字段 |
| --- | --- | --- |
| `usage` | `info` | `syntax` |
| `package_root` | `info` | `package_root` |
| `package_root_not_found` | `error` | `error`, `hint` |
| `summary` | `info` | `ok` |

### 包扫描与驱动包事件

| `event` | `level` | `device` | `data` 字段 |
| --- | --- | --- | --- |
| `package_dir_not_found` | `error` | 必填 | `package_dir` |
| `package_dir_scan_failed` | `error` | 必填 | `package_dir`, `error` |
| `inf_not_found` | `error` | 必填 | `package_dir` |
| `multiple_inf_found` | `error` | 必填 | `package_dir`, `files` |
| `package_present` | `info` | 必填 | `source_inf` |
| `package_staged` | `info` | 必填 | `published_inf` |
| `package_uninstalled` | `info` | 必填 | `source_inf`, `need_reboot` |
| `package_stage_failed` | `error` | 必填 | `source_inf`, `error_code`, `error` |
| `package_uninstall_failed` | `error` | 必填 | `source_inf`, `error_code`, `error` |

### 设备整理事件

| `event` | `level` | `device` | `data` 字段 |
| --- | --- | --- | --- |
| `device_info_list_failed` | `error` | 必填 | `error_code`, `error` |
| `device_create_failed` | `error` | 必填 | `error_code`, `error` |
| `device_hwid_failed` | `error` | 必填 | `error_code`, `error` |
| `device_register_failed` | `error` | 必填 | `error_code`, `error` |
| `device_bind_failed` | `error` | 必填 | `source_inf`, `hardware_id`, `error_code`, `error` |
| `device_bound` | `info` | 必填 | `source_inf`, `hardware_id`, `need_reboot` |
| `device_created` | `info` | 必填 | `instance_id`, `instance_count`, `duplicate_count` |
| `device_kept` | `info` | 必填 | `instance_id`, `instance_count`, `duplicate_count` |
| `device_absent` | `info` | 必填 | `instance_count` |
| `device_removed` | `info` | 必填 | `instance_id`, `need_reboot` |
| `device_remove_failed` | `error` | 必填 | `instance_id`, `error` |

## 示例

### install

命令：

```powershell
devtidy.exe install
```

```json
{"data":{"package_root":"C:\\work\\devtidy-bundle"},"device":null,"event":"package_root","level":"info","schema":"v1"}
{"data":{"source_inf":"C:\\work\\devtidy-bundle\\YumeDiskSCSI\\YumeDiskSCSI.inf"},"device":"YumeDiskSCSI","event":"package_present","level":"info","schema":"v1"}
{"data":{"instance_id":"ROOT\\YUMEDISKSCSI\\0000","instance_count":2,"duplicate_count":1},"device":"YumeDiskSCSI","event":"device_kept","level":"info","schema":"v1"}
{"data":{"instance_id":"ROOT\\YUMEDISKSCSI\\0001","need_reboot":false},"device":"YumeDiskSCSI","event":"device_removed","level":"info","schema":"v1"}
{"data":{"ok":true},"device":null,"event":"summary","level":"info","schema":"v1"}
```

### uninstall

命令：

```powershell
devtidy.exe uninstall
```

若要显式指定驱动包目录：

```powershell
devtidy.exe uninstall --package-root C:\work\devtidy-bundle
```

示例输出：

```json
{"data":{"package_root":"C:\\work\\devtidy-bundle"},"device":null,"event":"package_root","level":"info","schema":"v1"}
{"data":{"instance_id":"ROOT\\YUMEDISKSCSI\\0000","need_reboot":false},"device":"YumeDiskSCSI","event":"device_removed","level":"info","schema":"v1"}
{"data":{"source_inf":"C:\\work\\devtidy-bundle\\YumeDiskSCSI\\YumeDiskSCSI.inf","need_reboot":false},"device":"YumeDiskSCSI","event":"package_uninstalled","level":"info","schema":"v1"}
{"data":{"instance_id":"ROOT\\YUMEDISKKMDF\\0000","need_reboot":false},"device":"YumeDiskKMDF","event":"device_removed","level":"info","schema":"v1"}
{"data":{"source_inf":"C:\\work\\devtidy-bundle\\YumeDiskKMDF\\YumeDiskKMDF.inf","need_reboot":false},"device":"YumeDiskKMDF","event":"package_uninstalled","level":"info","schema":"v1"}
{"data":{"ok":true},"device":null,"event":"summary","level":"info","schema":"v1"}
```

若某类设备当前不存在，则不会报错，而是输出：

```json
{"data":{"instance_count":0},"device":"YumeDiskSCSI","event":"device_absent","level":"info","schema":"v1"}
```
