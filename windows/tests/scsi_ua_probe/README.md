# scsi_ua_probe

一个最小 Windows 原生 SCSI probe，用于对 `\\.\PhysicalDriveN` 发：

- `TEST UNIT READY`
- `REQUEST SENSE`
- `READ CAPACITY(10)`

默认走 buffered `IOCTL_SCSI_PASS_THROUGH`，也支持 `direct` 对照模式。

## 构建

```powershell
cmake -S windows/tests -B build/windows-tests-vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/windows-tests-vs2022 --config Release --target scsi_ua_probe
```

## 示例

```powershell
build/windows-tests-vs2022/scsi_ua_probe/Release/scsi_ua_probe.exe --device \\.\PhysicalDrive2 --op tur --repeat 2
build/windows-tests-vs2022/scsi_ua_probe/Release/scsi_ua_probe.exe --device \\.\PhysicalDrive2 --op tur --mode direct
build/windows-tests-vs2022/scsi_ua_probe/Release/scsi_ua_probe.exe --device \\.\PhysicalDrive2 --op request-sense
build/windows-tests-vs2022/scsi_ua_probe/Release/scsi_ua_probe.exe --device \\.\PhysicalDrive2 --op read-capacity10
```

## 当前观察

- 对 `\\.\PhysicalDriveN`，`tur` 的 buffered/direct 模式都可稳定返回 `scsi_status`。
- 在本项目的 `Unit Attention` 验证里，target sibling 写入后的第一次 `tur` 会返回 `scsi_status=0x02`，第二次恢复 `0x00`。
- `request-sense` 在 `\\.\PhysicalDriveN` 上可能被 Windows disk class 直接拒绝并返回 `ERROR_INVALID_PARAMETER (87)`；因此当前黑盒 runtime 验证优先看 `tur` 的第一次 `CHECK CONDITION`。
