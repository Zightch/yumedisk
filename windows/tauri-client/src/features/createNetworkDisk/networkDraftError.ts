export function mapNetworkDraftError(error: unknown, fallback: string): string {
  const code = getErrorCode(error);
  switch (code) {
    case "network-connect-failed":
      return "测试连接失败";
    case "network-auth-failed":
      return "认证失败";
    case "network-session-open-failed":
      return "会话打开失败";
    case "network-metadata-failed":
      return "元数据获取失败";
    case "network-disk-duplicate":
      return "网络盘已存在";
    case "network-draft-empty":
      return "当前没有可提交的网络盘";
    case "network-draft-not-found":
      return "网络盘草稿不存在";
    case "network-draft-item-not-found":
      return "网络盘草稿项不存在";
    case "network-server-addr-empty":
      return "服务器地址不能为空";
    default:
      return fallback;
  }
}

function getErrorCode(error: unknown): string | null {
  if (error && typeof error === "object") {
    const code = Reflect.get(error, "code");
    if (typeof code === "string" && code.length > 0) {
      return code;
    }
  }

  return null;
}
