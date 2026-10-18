export type Id = string;
export interface User {
  id: Id;
  address: string;
}
export interface Mailbox {
  id: Id;
  name: string;
  box_type: number;
  total: number;
  unread: number;
}
export interface Message {
  id: Id;
  subject: string;
  sender: string;
  recipients: string;
  send_time: number;
  status: number;
  is_starred: boolean;
  has_attachment: boolean;
}
export interface Page {
  mailbox: Mailbox;
  total: number;
  page: number;
  page_size: number;
  total_pages: number;
  messages: Message[];
}
export interface Detail {
  message: Omit<Message, "recipients"> & { recipients: string[] };
  body_available: boolean;
  body: {
    text?: string;
    html?: string;
    text_available: boolean;
    html_available: boolean;
    text_truncated: boolean;
    html_truncated: boolean;
  };
  attachments: { id: Id; filename: string; size: number; mime_type: string }[];
}
export interface Draft {
  to: string;
  cc: string;
  subject: string;
  text: string;
  html: string;
  pendingVerification?: string;
}
export const blankDraft = (): Draft => ({
  to: "",
  cc: "",
  subject: "",
  text: "",
  html: "",
});
export class ApiError extends Error {
  constructor(
    public status: number,
    public payload: {
      error?: string;
      message_id?: string;
      stage?: string;
    } = {},
  ) {
    super(payload.error || `HTTP ${status}`);
  }
}
export async function request<T>(
  path: string,
  init: RequestInit = {},
  authEvent = true,
): Promise<T> {
  let response: Response;
  try {
    response = await fetch(path, {
      ...init,
      credentials: "same-origin",
      headers: {
        ...(init.body ? { "Content-Type": "application/json" } : {}),
        ...init.headers,
      },
    });
  } catch (error) {
    if ((error as Error).name === "AbortError") throw error;
    throw new ApiError(0, { error: "无法连接邮件服务，请检查网络后重试。" });
  }
  if (response.status === 401 && authEvent)
    window.dispatchEvent(new Event("webmail-session-expired"));
  const payload =
    init.method === "HEAD" ? {} : await response.json().catch(() => ({}));
  if (!response.ok) throw new ApiError(response.status, payload);
  return payload as T;
}
export const messagePath = (box: Id, mail: Id) =>
  `/api/mailboxes/${encodeURIComponent(box)}/messages/${encodeURIComponent(mail)}`;
export const attachmentPath = (box: Id, mail: Id, attachment: Id) =>
  `${messagePath(box, mail)}/attachments/${encodeURIComponent(attachment)}`;
export const setFlags = (
  box: Id,
  mail: Id,
  flags: { read?: boolean; is_starred?: boolean },
) =>
  request(messagePath(box, mail), {
    method: "PATCH",
    body: JSON.stringify(flags),
  });
export function errorText(error: unknown): string {
  if (!(error instanceof ApiError)) return "操作未完成，请稍后重试。";
  if (error.status === 401) return "登录已失效，请重新登录。";
  if (error.status === 404) return "这封邮件已不在当前邮箱中，请刷新列表。";
  if (error.status === 503) return "邮件服务暂不可用，请稍后重试。";
  if (error.status === 422)
    return error.payload.stage === "mail"
      ? "今日发信配额已用完，请稍后再试。"
      : "邮件提交被拒绝，请检查收件地址。";
  if (error.status === 413) return "邮件内容超过大小限制，请精简后重试。";
  return error.status === 0 ? error.message : "操作未完成，请检查输入后重试。";
}
export const splitAddresses = (value: string) =>
  value
    .split(/[,;，；\n]/)
    .map((x) => x.trim())
    .filter(Boolean);
