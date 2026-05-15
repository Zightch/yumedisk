import { h, onBeforeUnmount } from "vue";
import type { NotificationHandle } from "element-plus";
import { ElMessage, ElMessageBox, ElNotification } from "element-plus";
import type { HomeDiskListItem } from "../../entities/disk/model";
import {
  commitDeletedDisk,
  deleteDisk,
  undoDeleteDisk,
} from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/sessionClient";
import DeleteDiskNotification from "./DeleteDiskNotification.vue";

const DELETE_NOTIFICATION_DURATION_MS = 3400;
const DELETE_NOTIFICATION_LEAVE_MS = 450;

interface PendingDeleteNotice {
  deletionId: string;
  diskName: string;
  handle: NotificationHandle;
  commitTimerId: number | null;
  undoing: boolean;
  undone: boolean;
}

export function useRemoveDiskFlow(options: {
  actionLoadingDiskId: { value: string | null };
  loadHomeDiskList: (options?: { showLoading?: boolean }) => Promise<unknown>;
}) {
  const pendingNotices = new Map<string, PendingDeleteNotice>();

  function clearCommitTimer(notice: PendingDeleteNotice): void {
    if (notice.commitTimerId !== null) {
      window.clearTimeout(notice.commitTimerId);
      notice.commitTimerId = null;
    }
  }

  function forgetNotice(deletionId: string): void {
    const notice = pendingNotices.get(deletionId);
    if (!notice) {
      return;
    }

    clearCommitTimer(notice);
    pendingNotices.delete(deletionId);
  }

  function scheduleDeleteCommit(notice: PendingDeleteNotice): void {
    if (notice.undone || notice.commitTimerId !== null) {
      return;
    }

    // Element Plus fires onClose before the leave transition completes.
    notice.commitTimerId = window.setTimeout(async () => {
      notice.commitTimerId = null;
      pendingNotices.delete(notice.deletionId);

      try {
        await commitDeletedDisk({ deletionId: notice.deletionId });
      } catch (error) {
        const message = getErrorMessage(error);
        if (message !== "删除撤销窗口已结束") {
          ElMessage.error(message);
        }
      }
    }, DELETE_NOTIFICATION_LEAVE_MS);
  }

  async function handleUndoDelete(deletionId: string): Promise<void> {
    const notice = pendingNotices.get(deletionId);
    if (!notice || notice.undoing || notice.undone) {
      return;
    }

    notice.undoing = true;
    clearCommitTimer(notice);

    try {
      await undoDeleteDisk({ deletionId });
      notice.undone = true;
      forgetNotice(deletionId);
      notice.handle.close();
      await options.loadHomeDiskList({ showLoading: false });
      ElMessage.success("已撤销删除");
    } catch (error) {
      notice.undoing = false;
      ElMessage.error(getErrorMessage(error));
    }
  }

  function showDeleteNotification(disk: HomeDiskListItem, deletionId: string): void {
    const notice = {} as PendingDeleteNotice;

    const handle = ElNotification({
      title: "删除成功",
      position: "top-right",
      duration: DELETE_NOTIFICATION_DURATION_MS,
      customClass: "remove-disk-notification-shell",
      message: h(DeleteDiskNotification, {
        diskName: disk.diskName,
        onUndo: () => {
          void handleUndoDelete(deletionId);
        },
      }),
      onClose: () => {
        scheduleDeleteCommit(notice);
      },
    });

    notice.deletionId = deletionId;
    notice.diskName = disk.diskName;
    notice.handle = handle;
    notice.commitTimerId = null;
    notice.undoing = false;
    notice.undone = false;
    pendingNotices.set(deletionId, notice);
  }

  async function removeDisk(disk: HomeDiskListItem): Promise<void> {
    try {
      await ElMessageBox.confirm(
        `确认删除 ${disk.diskName}？删除后可以在顶部通知中撤销。`,
        "确认删除",
        {
          type: "warning",
          confirmButtonText: "删除",
          cancelButtonText: "取消",
          autofocus: false,
          closeOnClickModal: false,
          closeOnPressEscape: true,
        },
      );
    } catch (error) {
      if (error === "cancel" || error === "close") {
        return;
      }
      throw error;
    }

    options.actionLoadingDiskId.value = disk.diskId;

    try {
      const response = await deleteDisk({ diskId: disk.diskId });
      await options.loadHomeDiskList({ showLoading: false });

      if (response.undoAvailable) {
        showDeleteNotification(disk, response.deletionId);
        return;
      }

      ElMessage.success("磁盘已删除");
    } catch (error) {
      ElMessage.error(getErrorMessage(error));
    } finally {
      options.actionLoadingDiskId.value = null;
    }
  }

  onBeforeUnmount(() => {
    for (const notice of pendingNotices.values()) {
      clearCommitTimer(notice);
      notice.handle.close();
    }
    pendingNotices.clear();
  });

  return {
    removeDisk,
  };
}
