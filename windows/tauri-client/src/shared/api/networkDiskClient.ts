import { invoke } from "@tauri-apps/api/core";
import type {
  AddNetworkDraftItemRequest,
  CreateNetworkDraftRequest,
  DisposeNetworkDraftRequest,
  NetworkDraftSnapshot,
  RemoveNetworkDraftItemRequest,
  SubmitNetworkDraftRequest,
  TestNetworkConnectionRequest,
} from "../../entities/disk/model";

export async function testNetworkConnection(
  request: TestNetworkConnectionRequest,
): Promise<void> {
  return invoke<void>("test_network_connection", { request });
}

export async function createNetworkDraft(
  request: CreateNetworkDraftRequest,
): Promise<NetworkDraftSnapshot> {
  return invoke<NetworkDraftSnapshot>("create_network_draft", { request });
}

export async function addNetworkDraftItem(
  request: AddNetworkDraftItemRequest,
): Promise<NetworkDraftSnapshot> {
  return invoke<NetworkDraftSnapshot>("add_network_draft_item", { request });
}

export async function removeNetworkDraftItem(
  request: RemoveNetworkDraftItemRequest,
): Promise<NetworkDraftSnapshot> {
  return invoke<NetworkDraftSnapshot>("remove_network_draft_item", { request });
}

export async function submitNetworkDraft(
  request: SubmitNetworkDraftRequest,
): Promise<void> {
  return invoke<void>("submit_network_draft", { request });
}

export async function disposeNetworkDraft(
  request: DisposeNetworkDraftRequest,
): Promise<void> {
  return invoke<void>("dispose_network_draft", { request });
}
