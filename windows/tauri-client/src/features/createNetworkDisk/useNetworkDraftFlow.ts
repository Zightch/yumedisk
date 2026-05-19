import { ElMessage } from "element-plus";
import { computed, reactive, ref, watch, type Ref } from "vue";
import type { NetworkDraftItem } from "../../entities/disk/model";
import {
  addNetworkDraftItem,
  createNetworkDraft,
  disposeNetworkDraft,
  removeNetworkDraftItem,
  submitNetworkDraft,
} from "../../shared/api/networkDiskClient";
import { mapNetworkDraftError } from "./networkDraftError";

interface UseNetworkDraftFlowOptions {
  visible: Ref<boolean>;
  closeDialog: () => void;
  onCreated: () => void;
}

export function useNetworkDraftFlow(options: UseNetworkDraftFlowOptions) {
  const form = reactive({
    serverAddr: "",
    diskName: "",
    claimCode: "",
  });

  const testing = ref(false);
  const adding = ref(false);
  const submitting = ref(false);
  const disposingDraft = ref(false);
  const removingRemoteDiskId = ref<string | null>(null);
  const errorText = ref<string | null>(null);
  const draftId = ref<string | null>(null);
  const draftServerAddr = ref("");
  const draftItems = ref<NetworkDraftItem[]>([]);

  const normalizedServerAddr = computed(() => form.serverAddr.trim());
  const canEditDraft = computed(
    () =>
      draftId.value !== null &&
      !testing.value &&
      !submitting.value &&
      !adding.value &&
      removingRemoteDiskId.value === null,
  );
  const canAddItem = computed(
    () =>
      canEditDraft.value &&
      form.diskName.trim().length > 0 &&
      form.claimCode.trim().length > 0,
  );
  const canSubmit = computed(
    () =>
      draftId.value !== null &&
      draftItems.value.length > 0 &&
      !testing.value &&
      !adding.value &&
      removingRemoteDiskId.value === null &&
      !submitting.value,
  );
  const connectionStatusText = computed(() => {
    if (testing.value) {
      return "测试连接中";
    }

    if (draftId.value) {
      return `已连接 · ${draftServerAddr.value}`;
    }

    return "尚未测试";
  });
  const connectionStatusClass = computed(() => {
    if (testing.value) {
      return "network-dialog__status-badge--testing";
    }

    if (draftId.value) {
      return "network-dialog__status-badge--ready";
    }

    return "network-dialog__status-badge--idle";
  });

  watch(options.visible, (visible) => {
    if (visible) {
      if (!draftId.value) {
        resetAllState();
      }
      return;
    }

    void closeDraftSession();
  });

  watch(normalizedServerAddr, (nextServerAddr) => {
    if (!draftId.value || disposingDraft.value) {
      return;
    }

    if (nextServerAddr === draftServerAddr.value) {
      return;
    }

    void resetDraftForServerChange();
  });

  function resetAllState() {
    form.serverAddr = "";
    form.diskName = "";
    form.claimCode = "";
    errorText.value = null;
    testing.value = false;
    adding.value = false;
    submitting.value = false;
    disposingDraft.value = false;
    removingRemoteDiskId.value = null;
    clearDraftState();
  }

  function resetDraftInputs() {
    form.diskName = "";
    form.claimCode = "";
    errorText.value = null;
    adding.value = false;
    removingRemoteDiskId.value = null;
    draftItems.value = [];
  }

  function clearDraftState() {
    draftId.value = null;
    draftServerAddr.value = "";
    draftItems.value = [];
  }

  function validateServerAddr(): string | null {
    if (normalizedServerAddr.value.length === 0) {
      return "服务器地址不能为空";
    }

    return null;
  }

  function validateDraftItem(): string | null {
    if (form.diskName.trim().length === 0) {
      return "磁盘名称不能为空";
    }

    if (form.claimCode.trim().length === 0) {
      return "领盘码不能为空";
    }

    return null;
  }

  async function closeDraftSession() {
    if (!draftId.value) {
      resetAllState();
      return;
    }

    const disposed = await disposeCurrentDraft();
    if (disposed) {
      resetAllState();
    }
  }

  async function resetDraftForServerChange() {
    if (!draftId.value) {
      return;
    }

    const previousServerAddr = draftServerAddr.value;
    const disposed = await disposeCurrentDraft();
    if (!disposed) {
      form.serverAddr = previousServerAddr;
      return;
    }

    resetDraftInputs();
  }

  async function disposeCurrentDraft(): Promise<boolean> {
    if (!draftId.value || disposingDraft.value) {
      return draftId.value === null;
    }

    const currentDraftId = draftId.value;
    disposingDraft.value = true;
    let disposed = false;

    try {
      await disposeNetworkDraft({ draftId: currentDraftId });
      disposed = true;
    } catch {
      errorText.value = "网络盘草稿清理失败";
      ElMessage.error("网络盘草稿清理失败");
    } finally {
      disposingDraft.value = false;
      if (disposed && draftId.value === currentDraftId) {
        clearDraftState();
      }
    }

    return disposed;
  }

  async function handleTestConnection() {
    const error = validateServerAddr();
    if (error) {
      errorText.value = error;
      ElMessage.error(error);
      return;
    }

    const serverAddr = normalizedServerAddr.value;
    if (draftId.value && draftServerAddr.value === serverAddr) {
      errorText.value = null;
      ElMessage.success("测试连接成功");
      return;
    }

    testing.value = true;
    try {
      if (draftId.value) {
        const disposed = await disposeCurrentDraft();
        if (!disposed) {
          return;
        }
      }

      const snapshot = await createNetworkDraft({ serverAddr });
      if (!options.visible.value) {
        await disposeNetworkDraft({ draftId: snapshot.draftId }).catch(() => {});
        return;
      }

      draftId.value = snapshot.draftId;
      draftServerAddr.value = snapshot.serverAddr;
      draftItems.value = snapshot.items;
      errorText.value = null;
      ElMessage.success("测试连接成功");
    } catch {
      errorText.value = "测试连接失败";
      ElMessage.error("测试连接失败");
    } finally {
      testing.value = false;
    }
  }

  async function handleAddDraftItem() {
    const error = validateDraftItem();
    if (error) {
      errorText.value = error;
      ElMessage.error(error);
      return;
    }

    if (!draftId.value) {
      errorText.value = "请先测试连接";
      ElMessage.error("请先测试连接");
      return;
    }

    adding.value = true;
    try {
      const snapshot = await addNetworkDraftItem({
        draftId: draftId.value,
        diskName: form.diskName.trim(),
        claimCode: form.claimCode.trim(),
      });
      if (!options.visible.value) {
        return;
      }

      draftItems.value = snapshot.items;
      form.diskName = "";
      form.claimCode = "";
      errorText.value = null;
    } catch (error) {
      const message = mapNetworkDraftError(error, "添加网络盘失败");
      errorText.value = message;
      ElMessage.error(message);
    } finally {
      adding.value = false;
    }
  }

  async function handleRemoveDraftItem(remoteDiskId: string) {
    if (!draftId.value) {
      return;
    }

    removingRemoteDiskId.value = remoteDiskId;
    try {
      const snapshot = await removeNetworkDraftItem({
        draftId: draftId.value,
        remoteDiskId,
      });
      if (!options.visible.value) {
        return;
      }

      draftItems.value = snapshot.items;
      errorText.value = null;
    } catch {
      ElMessage.error("移除网络盘失败");
    } finally {
      removingRemoteDiskId.value = null;
    }
  }

  async function handleSubmit() {
    if (!draftId.value) {
      errorText.value = "请先测试连接";
      ElMessage.error("请先测试连接");
      return;
    }

    if (draftItems.value.length === 0) {
      errorText.value = "当前没有可提交的网络盘";
      ElMessage.error("当前没有可提交的网络盘");
      return;
    }

    submitting.value = true;
    try {
      await submitNetworkDraft({ draftId: draftId.value });
      clearDraftState();
      options.onCreated();
      ElMessage.success("网络盘已提交");
      options.closeDialog();
    } catch (error) {
      const message = mapNetworkDraftError(error, "提交网络盘失败");
      errorText.value = message;
      ElMessage.error(message);
    } finally {
      submitting.value = false;
    }
  }

  function handleCancel() {
    options.closeDialog();
  }

  return {
    adding,
    canAddItem,
    canEditDraft,
    canSubmit,
    connectionStatusClass,
    connectionStatusText,
    draftItems,
    errorText,
    form,
    handleAddDraftItem,
    handleCancel,
    handleRemoveDraftItem,
    handleSubmit,
    handleTestConnection,
    removingRemoteDiskId,
    submitting,
    testing,
  };
}
