import {
  lazy,
  Suspense,
  useCallback,
  useEffect,
  useRef,
  useState,
  type FormEvent,
} from "react";
import {
  Mail,
  Inbox,
  Send,
  FileText,
  Trash2,
  CircleAlert,
  ChevronRight,
  ChevronLeft,
  RefreshCw,
  Search,
  PenLine,
  LogOut,
  ArrowLeft,
  Star,
  Paperclip,
  Download,
  X,
  Check,
  ShieldCheck,
  ArrowUpRight,
  LoaderCircle,
  AlertCircle,
  Menu,
  Eye,
  EyeOff,
} from "lucide-react";
const Compose = lazy(() => import("./Compose"));
import {
  ApiError,
  attachmentPath,
  blankDraft,
  errorText,
  messagePath,
  request,
  setFlags,
  type Detail,
  type Draft,
  type Id,
  type Mailbox,
  type Message,
  type Page,
  type User,
} from "./api";

const boxName = (box: Mailbox) =>
  box.box_type === 1 ? "收件箱" : box.box_type === 2 ? "已发送" : box.name;
const displayAddress = (value: string) =>
  value ? value.split("@")[0] : "未知发件人";
function dateLabel(time: number, full = false) {
  return new Intl.DateTimeFormat(
    "zh-CN",
    full
      ? {
          year: "numeric",
          month: "long",
          day: "numeric",
          hour: "2-digit",
          minute: "2-digit",
        }
      : { month: "2-digit", day: "2-digit" },
  ).format(time * 1000);
}
function formatSize(size: number) {
  return size < 1024
    ? `${size} B`
    : size < 1024 * 1024
      ? `${(size / 1024).toFixed(1)} KB`
      : `${(size / 1024 / 1024).toFixed(1)} MB`;
}
function FolderIcon({ box }: { box: Mailbox }) {
  const Icon =
    box.box_type === 1
      ? Inbox
      : box.box_type === 2
        ? Send
        : /草稿|draft/i.test(box.name)
          ? FileText
          : /删|trash/i.test(box.name)
            ? Trash2
            : CircleAlert;
  return <Icon size={18} strokeWidth={1.7} />;
}
function htmlDocument(html: string) {
  return `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="referrer" content="no-referrer"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'none'; style-src 'unsafe-inline'; img-src data:; base-uri 'none'; form-action 'none'"><style>body{font:15px/1.9 system-ui,sans-serif;color:#344139;margin:0;padding:12px 2px;overflow-wrap:anywhere}pre{white-space:pre-wrap}a{color:#47694e}img{max-width:100%}blockquote{border-left:3px solid #dce3d8;margin:16px 0;padding-left:18px;color:#687469}table{max-width:100%}</style></head><body>${html}</body></html>`;
}

function Login({
  busy,
  notice,
  onLogin,
}: {
  busy: boolean;
  notice: string;
  onLogin: (address: string, password: string) => Promise<void>;
}) {
  const [address, setAddress] = useState("");
  const [password, setPassword] = useState("");
  const [error, setError] = useState("");
  const [showPassword, setShowPassword] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  async function submit(e: FormEvent) {
    e.preventDefault();
    if (submitting) return;
    setSubmitting(true);
    setError("");
    try {
      await onLogin(address.trim(), password);
      setPassword("");
    } catch (err) {
      setError(
        err instanceof ApiError && err.status === 401
          ? "邮箱或密码不正确，请重新输入。"
          : errorText(err),
      );
    } finally {
      setSubmitting(false);
    }
  }
  return (
    <div className="login-page">
      <div className="login-story">
        <div className="brand light-brand">
          <span className="brand-mark">
            <Mail size={23} />
          </span>
          ProtoRelay<span className="brand-caption">MAIL</span>
        </div>
        <div className="story-body">
          <span className="story-label">为每一封值得的来信</span>
          <h1>
            消息有来处，
            <br />
            思绪有归处。
          </h1>
          <p>
            一个安静、清晰的空间。
            <br />
            让沟通回归简单，让重要的事不被错过。
          </p>
          <div className="letter-art" aria-hidden="true">
            <div className="letter-back" />
            <div className="letter-paper">
              <span />
              <span />
              <span />
            </div>
            <div className="letter-front">
              <Mail size={40} strokeWidth={1} />
              <i />
            </div>
            <div className="letter-stamp">
              <Check size={17} />
            </div>
          </div>
        </div>
        <div className="story-footer">
          ProtoRelay Webmail <span>让每一次沟通，从容发生。</span>
        </div>
      </div>
      <main className="login-main">
        <div className="login-card">
          <span className="eyebrow">YOUR PERSONAL MAILBOX</span>
          <h2>欢迎回来</h2>
          <p className="login-intro">登录邮箱，查看你的最新消息。</p>
          {notice && (
            <div className="login-notice" role="status">
              {notice}
            </div>
          )}
          <form onSubmit={submit}>
            <label>
              邮箱地址
              <input
                type="email"
                name="username"
                autoComplete="username"
                required
                value={address}
                onChange={(e) => setAddress(e.target.value)}
                placeholder="you@example.com"
                disabled={submitting || busy}
              />
            </label>
            <label>
              密码
              <div className="password-input">
                <input
                  type={showPassword ? "text" : "password"}
                  name="password"
                  autoComplete="current-password"
                  required
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                  placeholder="输入你的邮箱密码"
                  disabled={submitting || busy}
                />
                <button
                  type="button"
                  aria-label={showPassword ? "隐藏密码" : "显示密码"}
                  onClick={() => setShowPassword(!showPassword)}
                >
                  {showPassword ? <EyeOff size={17} /> : <Eye size={17} />}
                </button>
              </div>
            </label>
            {error && (
              <div className="inline-error" role="alert">
                <AlertCircle size={16} />
                {error}
              </div>
            )}
            <button
              className="primary-button login-button"
              disabled={submitting || busy}
            >
              {busy || submitting ? (
                <LoaderCircle className="spin" size={18} />
              ) : (
                <ArrowUpRight size={18} />
              )}
              {busy
                ? "正在检查登录状态…"
                : submitting
                  ? "正在登录…"
                  : "登录邮箱"}
            </button>
          </form>
          <div className="login-policy">
            <ShieldCheck size={16} />
            <span>新登录会使该账号的旧会话失效</span>
          </div>
        </div>
        <footer>© {new Date().getFullYear()} ProtoRelay</footer>
      </main>
    </div>
  );
}

function Workspace({
  user,
  onLogout,
  onWrite,
  refreshVersion,
  notify,
}: {
  user: User;
  onLogout: () => void;
  onWrite: (to?: string) => void;
  refreshVersion: number;
  notify: (value: string) => void;
}) {
  const [boxes, setBoxes] = useState<Mailbox[]>([]);
  const [boxId, setBoxId] = useState<Id>("");
  const [page, setPage] = useState(1);
  const [data, setData] = useState<Page | null>(null);
  const [selectedId, setSelectedId] = useState<Id>("");
  const [detail, setDetail] = useState<Detail | null>(null);
  const [loading, setLoading] = useState(true);
  const [reading, setReading] = useState(false);
  const [error, setError] = useState("");
  const [readError, setReadError] = useState("");
  const [filter, setFilter] = useState("");
  const [revision, setRevision] = useState(0);
  const [sidebar, setSidebar] = useState(false);
  const [plain, setPlain] = useState(false);
  const [pendingFlags, setPendingFlags] = useState<Set<Id>>(new Set());
  const current = boxes.find((box) => box.id === boxId);
  const sent = current?.box_type === 2;
  const selection = useRef({ boxId, selectedId });
  selection.current = { boxId, selectedId };
  const refresh = useCallback(() => {
    setRevision((value) => value + 1);
  }, []);
  useEffect(() => {
    const focus = () => refresh();
    window.addEventListener("focus", focus);
    return () => window.removeEventListener("focus", focus);
  }, [refresh]);
  useEffect(() => {
    const controller = new AbortController();
    request<{ mailboxes: Mailbox[] }>("/api/mailboxes", {
      signal: controller.signal,
    })
      .then((out) => {
        setBoxes(out.mailboxes);
        if (!out.mailboxes.length) {
          setLoading(false);
          setError("该账号还没有邮箱，请联系管理员初始化系统邮箱。");
        }
        setBoxId((id) =>
          out.mailboxes.some((box) => box.id === id)
            ? id
            : out.mailboxes.find((box) => box.box_type === 1)?.id ||
              out.mailboxes[0]?.id ||
              "",
        );
      })
      .catch((err) => {
        if (err.name !== "AbortError") {
          setError(errorText(err));
          setLoading(false);
        }
      });
    return () => controller.abort();
  }, [revision, refreshVersion]);
  useEffect(() => {
    if (!boxId) return;
    const controller = new AbortController();
    setLoading(true);
    setError("");
    request<Page>(
      `/api/mailboxes/${boxId}/messages?page=${page}&page_size=20`,
      { signal: controller.signal },
    )
      .then((out) => {
        setData(out);
        if (out.total_pages && page > out.total_pages) setPage(out.total_pages);
      })
      .catch((err) => {
        if (err.name !== "AbortError") setError(errorText(err));
      })
      .finally(() => {
        if (!controller.signal.aborted) setLoading(false);
      });
    return () => controller.abort();
  }, [boxId, page, revision, refreshVersion]);
  useEffect(() => {
    if (!boxId || !selectedId) {
      setDetail(null);
      return;
    }
    const controller = new AbortController();
    setReading(true);
    setDetail(null);
    setReadError("");
    setPlain(false);
    request<Detail>(messagePath(boxId, selectedId), {
      signal: controller.signal,
    })
      .then(async (out) => {
        if (controller.signal.aborted) return;
        setDetail(out);
        if (out.message.status === 1 && !sent) {
          try {
            await setFlags(boxId, selectedId, { read: true });
            if (!controller.signal.aborted) {
              setDetail((value) =>
                value
                  ? { ...value, message: { ...value.message, status: 0 } }
                  : value,
              );
              setData((value) =>
                value
                  ? {
                      ...value,
                      messages: value.messages.map((m) =>
                        m.id === selectedId ? { ...m, status: 0 } : m,
                      ),
                    }
                  : value,
              );
              setBoxes((value) =>
                value.map((b) =>
                  b.id === boxId
                    ? { ...b, unread: Math.max(0, b.unread - 1) }
                    : b,
                ),
              );
            }
          } catch (err) {
            if (!controller.signal.aborted)
              notify(`已打开邮件，但未能标记已读：${errorText(err)}`);
          }
        }
      })
      .catch((err) => {
        if (err.name !== "AbortError") setReadError(errorText(err));
      })
      .finally(() => {
        if (!controller.signal.aborted) setReading(false);
      });
    return () => controller.abort();
  }, [boxId, selectedId]);
  function chooseBox(id: Id) {
    setBoxId(id);
    setPage(1);
    setSelectedId("");
    setDetail(null);
    setData(null);
    setError("");
    setFilter("");
    setSidebar(false);
  }
  async function flag(
    message: Message | Detail["message"],
    flags: { is_starred?: boolean; read?: boolean },
  ) {
    if (pendingFlags.has(message.id)) return;
    const origin = boxId;
    setPendingFlags((value) => new Set(value).add(message.id));
    try {
      await setFlags(origin, message.id, flags);
      if (selection.current.boxId !== origin) return;
      setData((value) =>
        value
          ? {
              ...value,
              messages: value.messages.map((item) =>
                item.id === message.id
                  ? {
                      ...item,
                      ...(flags.is_starred !== undefined
                        ? { is_starred: flags.is_starred }
                        : {}),
                      ...(flags.read !== undefined
                        ? { status: flags.read ? 0 : 1 }
                        : {}),
                    }
                  : item,
              ),
            }
          : value,
      );
      if (selection.current.selectedId === message.id)
        setDetail((value) =>
          value
            ? {
                ...value,
                message: {
                  ...value.message,
                  ...(flags.is_starred !== undefined
                    ? { is_starred: flags.is_starred }
                    : {}),
                  ...(flags.read !== undefined
                    ? { status: flags.read ? 0 : 1 }
                    : {}),
                },
              }
            : value,
        );
      if (flags.read !== undefined) {
        setBoxes((value) =>
          value.map((box) =>
            box.id === origin
              ? {
                  ...box,
                  unread: Math.max(0, box.unread + (flags.read ? -1 : 1)),
                }
              : box,
          ),
        );
      }
    } catch (err) {
      notify(errorText(err));
    } finally {
      setPendingFlags((value) => {
        const next = new Set(value);
        next.delete(message.id);
        return next;
      });
    }
  }
  async function download(id: Id) {
    try {
      const path = attachmentPath(boxId, selectedId, id);
      await request(path, { method: "HEAD" });
      const link = document.createElement("a");
      link.href = path;
      link.download = "";
      document.body.append(link);
      link.click();
      link.remove();
    } catch (err) {
      notify(errorText(err));
    }
  }
  const visible = (data?.messages || []).filter((m) =>
    `${m.subject} ${m.sender} ${m.recipients}`
      .toLowerCase()
      .includes(filter.toLowerCase()),
  );
  return (
    <div className={`workspace ${selectedId ? "has-selection" : ""}`}>
      {sidebar && (
        <button
          aria-label="关闭邮箱导航"
          className="mobile-shade"
          onClick={() => setSidebar(false)}
        />
      )}
      <aside className={`sidebar ${sidebar ? "visible" : ""}`}>
        <div className="brand">
          <span className="brand-mark">
            <Mail size={21} />
          </span>
          ProtoRelay
        </div>
        <span className="sidebar-caption">你的日常沟通空间</span>
        <button
          className="primary-button write-button"
          onClick={() => onWrite()}
        >
          <PenLine size={18} />
          写邮件<span>N</span>
        </button>
        <div className="folder-label">我的邮箱</div>
        <nav aria-label="邮箱导航">
          {boxes.map((box) => (
            <button
              key={box.id}
              className={`folder ${box.id === boxId ? "active" : ""}`}
              onClick={() => chooseBox(box.id)}
              aria-current={box.id === boxId ? "page" : undefined}
            >
              <FolderIcon box={box} />
              <span>{boxName(box)}</span>
              {box.unread > 0 && <b>{box.unread}</b>}
            </button>
          ))}
        </nav>
        <div className="sidebar-bottom">
          <div className="quiet-note">
            <span className="status-dot" />
            让沟通，回归简单。
          </div>
          <div className="profile">
            <span className="avatar small-avatar">
              {user.address[0].toUpperCase()}
            </span>
            <div>
              <strong>{displayAddress(user.address)}</strong>
              <span title={user.address}>{user.address}</span>
            </div>
            <button
              className="icon-button"
              aria-label="退出登录"
              title="退出登录"
              onClick={onLogout}
            >
              <LogOut size={17} />
            </button>
          </div>
        </div>
      </aside>
      <main className="mail-main">
        <header className="app-header">
          <div className="breadcrumb">
            <button
              className="icon-button mobile-menu"
              aria-label="打开邮箱导航"
              onClick={() => setSidebar(true)}
            >
              <Menu size={20} />
            </button>
            <Mail size={17} />
            <span>邮箱</span>
            <ChevronRight size={14} />
            <strong>{current ? boxName(current) : "加载中"}</strong>
          </div>
          <span className="header-address">
            {user.address}
            <span className="online-dot" />
          </span>
        </header>
        <div className="mail-columns">
          <section className="message-list" aria-label="邮件列表">
            <div className="list-heading">
              <div>
                <span className="eyebrow">
                  {sent ? "SENT MESSAGES" : "YOUR MESSAGES"}
                </span>
                <h1>
                  {current ? boxName(current) : "邮箱"}
                  <span>{data?.total ?? current?.total ?? 0}</span>
                </h1>
              </div>
              <button
                className="icon-button refresh-button"
                aria-label="刷新邮件"
                disabled={loading}
                onClick={refresh}
              >
                <RefreshCw size={18} className={loading ? "spin" : ""} />
              </button>
            </div>
            <div className="filter-input">
              <Search size={16} />
              <input
                type="search"
                aria-label="筛选当前页邮件"
                placeholder="筛选当前页邮件…"
                value={filter}
                onChange={(e) => setFilter(e.target.value)}
              />
            </div>
            <div className="list-caption">
              <span>按时间排列</span>
              {current && current.unread > 0 && (
                <span>{current.unread} 封未读</span>
              )}
            </div>
            {error ? (
              <div className="state-message error-state" role="alert">
                <CircleAlert size={27} />
                <strong>暂时无法加载邮件</strong>
                <p>{error}</p>
                <button className="secondary-button" onClick={refresh}>
                  重新加载
                </button>
              </div>
            ) : loading ? (
              <div className="skeleton-list" aria-label="正在加载邮件">
                {[0, 1, 2, 3, 4].map((i) => (
                  <div className="skeleton-row" key={i}>
                    <i />
                    <div>
                      <span />
                      <span />
                    </div>
                  </div>
                ))}
              </div>
            ) : !visible.length ? (
              <div className="state-message">
                <Inbox size={32} />
                <strong>
                  {filter ? "当前页没有匹配的邮件" : "这里还没有邮件"}
                </strong>
                <p>
                  {filter
                    ? "试试其他关键词，或清除筛选。"
                    : "收到的新消息会出现在这里。"}
                </p>
                {filter && (
                  <button className="text-button" onClick={() => setFilter("")}>
                    清除筛选
                  </button>
                )}
              </div>
            ) : (
              <ul className="mail-items">
                {visible.map((message) => {
                  const unread = message.status === 1 && !sent;
                  const from = sent
                    ? message.recipients || "收件人"
                    : message.sender;
                  return (
                    <li
                      key={message.id}
                      className={`mail-item ${selectedId === message.id ? "selected" : ""} ${unread ? "unread" : ""}`}
                    >
                      <button
                        className="mail-open"
                        aria-label={`打开邮件：${message.subject || "无主题"}`}
                        onClick={() => setSelectedId(message.id)}
                      >
                        <div className="mail-row-top">
                          <span className="sender-name">
                            {unread && <i className="unread-dot" />}
                            {displayAddress(from)}
                          </span>
                          <time
                            dateTime={new Date(
                              message.send_time * 1000,
                            ).toISOString()}
                          >
                            {dateLabel(message.send_time)}
                          </time>
                        </div>
                        <strong className="mail-subject">
                          {message.subject || "（无主题）"}
                        </strong>
                        <div className="mail-row-bottom">
                          <span>
                            {sent
                              ? `发给 ${message.recipients}`
                              : message.sender}
                          </span>
                          {message.has_attachment && <Paperclip size={13} />}
                        </div>
                      </button>
                      <button
                        className={`star-button ${message.is_starred ? "is-starred" : ""}`}
                        aria-label={`${message.is_starred ? "取消标星" : "标星"}：${message.subject || "无主题"}`}
                        disabled={pendingFlags.has(message.id)}
                        onClick={() =>
                          flag(message, { is_starred: !message.is_starred })
                        }
                      >
                        <Star
                          size={16}
                          fill={message.is_starred ? "currentColor" : "none"}
                        />
                      </button>
                    </li>
                  );
                })}
              </ul>
            )}
            <footer className="list-pagination">
              <span>
                第 {data?.page || 1} / {Math.max(1, data?.total_pages || 1)} 页
              </span>
              <div>
                <button
                  className="icon-button"
                  aria-label="上一页"
                  disabled={loading || page <= 1}
                  onClick={() => {
                    setPage(page - 1);
                    setSelectedId("");
                  }}
                >
                  <ChevronLeft size={17} />
                </button>
                <button
                  className="icon-button"
                  aria-label="下一页"
                  disabled={loading || page >= (data?.total_pages || 1)}
                  onClick={() => {
                    setPage(page + 1);
                    setSelectedId("");
                  }}
                >
                  <ChevronRight size={17} />
                </button>
              </div>
            </footer>
          </section>
          <section className="message-reader" aria-label="邮件阅读">
            <div className="reader-toolbar">
              <button
                className="icon-button reader-back"
                aria-label="返回邮件列表"
                onClick={() => setSelectedId("")}
              >
                <ArrowLeft size={18} />
              </button>
              <span>{selectedId ? "邮件详情" : "阅读空间"}</span>
              {detail && !reading && (
                <div className="reader-actions">
                  <button
                    className={`icon-button ${detail.message.is_starred ? "is-starred" : ""}`}
                    aria-label={
                      detail.message.is_starred
                        ? "取消当前邮件标星"
                        : "标星当前邮件"
                    }
                    disabled={pendingFlags.has(detail.message.id)}
                    onClick={() =>
                      flag(detail.message, {
                        is_starred: !detail.message.is_starred,
                      })
                    }
                  >
                    <Star
                      size={18}
                      fill={detail.message.is_starred ? "currentColor" : "none"}
                    />
                  </button>
                  {!sent && (
                    <button
                      className="icon-button"
                      aria-label={
                        detail.message.status === 1
                          ? "标记为已读"
                          : "标记为未读"
                      }
                      disabled={pendingFlags.has(detail.message.id)}
                      onClick={() =>
                        flag(detail.message, {
                          read: detail.message.status === 1,
                        })
                      }
                    >
                      <Mail size={18} />
                    </button>
                  )}
                </div>
              )}
            </div>
            {readError ? (
              <div className="state-message error-state" role="alert">
                <CircleAlert size={30} />
                <strong>无法打开这封邮件</strong>
                <p>{readError}</p>
              </div>
            ) : reading ? (
              <div className="state-message">
                <LoaderCircle size={28} className="spin" />
                <p>正在打开邮件…</p>
              </div>
            ) : !detail ? (
              <div className="reader-empty">
                <div className="empty-illustration">
                  <Mail size={43} strokeWidth={1.1} />
                  <span className="empty-tick">
                    <Check size={14} />
                  </span>
                </div>
                <h2>选一封邮件，开始阅读</h2>
                <p>给每一封来信，留一点专注的时间。</p>
                <button className="text-button" onClick={() => onWrite()}>
                  或者，写下新的问候 <ArrowUpRight size={15} />
                </button>
              </div>
            ) : (
              <article className="mail-detail">
                <div className="detail-tag">
                  {sent ? "已发送" : "来信"}
                  {detail.message.is_starred && (
                    <>
                      <span>·</span>
                      <Star size={12} fill="currentColor" />
                      已标星
                    </>
                  )}
                </div>
                <h2>{detail.message.subject || "（无主题）"}</h2>
                <div className="sender-block">
                  <span className="avatar">
                    {(detail.message.sender || "?")[0].toUpperCase()}
                  </span>
                  <div>
                    <div className="sender-title">
                      <strong>{displayAddress(detail.message.sender)}</strong>
                      <span>&lt;{detail.message.sender}&gt;</span>
                    </div>
                    <p>收件人：{detail.message.recipients.join("，")}</p>
                  </div>
                  <time>{dateLabel(detail.message.send_time, true)}</time>
                </div>
                {detail.body_available ? (
                  <>
                    <div className="body-switch">
                      {detail.body.html_available &&
                        detail.body.text_available && (
                          <button
                            className="text-button"
                            onClick={() => setPlain(!plain)}
                          >
                            {plain ? "查看排版视图" : "查看纯文本"}
                          </button>
                        )}
                    </div>
                    {plain || !detail.body.html_available ? (
                      <pre className="plain-body">
                        {detail.body.text ?? "这封邮件没有可展示的纯文本正文。"}
                      </pre>
                    ) : (
                      <iframe
                        className="html-body"
                        sandbox=""
                        referrerPolicy="no-referrer"
                        title="邮件 HTML 正文"
                        srcDoc={htmlDocument(detail.body.html || "")}
                      />
                    )}
                    {(detail.body.text_truncated ||
                      detail.body.html_truncated) && (
                      <p className="body-note">
                        正文较长，当前仅显示部分内容。
                      </p>
                    )}
                  </>
                ) : (
                  <div className="inline-error" role="alert">
                    正文暂不可用，邮件元数据仍可查看。
                  </div>
                )}
                {!!detail.attachments.length && (
                  <section className="attachments" aria-label="附件">
                    <h3>
                      <Paperclip size={15} />
                      附件 <span>{detail.attachments.length}</span>
                    </h3>
                    {detail.attachments.map((item) => (
                      <button
                        className="attachment"
                        key={item.id}
                        onClick={() => download(item.id)}
                        aria-label={`下载附件：${item.filename}`}
                      >
                        <span className="file-icon">
                          <FileText size={20} />
                        </span>
                        <span>
                          <strong>{item.filename}</strong>
                          <small>{formatSize(item.size)}</small>
                        </span>
                        <Download size={17} />
                      </button>
                    ))}
                  </section>
                )}
                {!sent && (
                  <div className="reply-footer">
                    <button
                      className="secondary-button"
                      onClick={() => onWrite(detail.message.sender)}
                    >
                      <PenLine size={15} />
                      给发件人写信
                    </button>
                  </div>
                )}
              </article>
            )}
          </section>
        </div>
      </main>
    </div>
  );
}

export default function App() {
  const [user, setUser] = useState<User | null>(null);
  const [checking, setChecking] = useState(true);
  const [notice, setNotice] = useState("");
  const [toast, setToast] = useState("");
  const [compose, setCompose] = useState(false);
  const [draft, setDraft] = useState<Draft>(blankDraft);
  const [owner, setOwner] = useState("");
  const [refresh, setRefresh] = useState(0);
  const notify = useCallback((text: string) => setToast(text), []);
  useEffect(() => {
    const expired = () => {
      setUser(null);
      setCompose(false);
      setNotice(
        "登录已失效，可能已在其他设备重新登录。未发送内容保留在当前页面。",
      );
    };
    window.addEventListener("webmail-session-expired", expired);
    return () => window.removeEventListener("webmail-session-expired", expired);
  }, []);
  useEffect(() => {
    const controller = new AbortController();
    request<{ user: User }>("/api/me", { signal: controller.signal }, false)
      .then((out) => {
        setUser(out.user);
        setOwner(out.user.address);
      })
      .catch((err) => {
        if (
          err.name !== "AbortError" &&
          (!(err instanceof ApiError) || err.status !== 401)
        )
          setNotice(errorText(err));
      })
      .finally(() => {
        if (!controller.signal.aborted) setChecking(false);
      });
    return () => controller.abort();
  }, []);
  useEffect(() => {
    if (!toast) return;
    const timer = setTimeout(() => setToast(""), 6500);
    return () => clearTimeout(timer);
  }, [toast]);
  async function login(address: string, password: string) {
    const out = await request<{ user: User }>(
      "/api/login",
      { method: "POST", body: JSON.stringify({ address, password }) },
      false,
    );
    if (owner && owner !== out.user.address) setDraft(blankDraft());
    setOwner(out.user.address);
    setUser(out.user);
    setNotice("");
  }
  async function logout() {
    try {
      await request("/api/logout", { method: "POST", body: "{}" });
      setUser(null);
      setCompose(false);
      setDraft(blankDraft());
      setNotice("你已退出登录。");
    } catch (err) {
      notify(errorText(err));
    }
  }
  function write(to = "") {
    if (to) setDraft((value) => ({ ...value, to }));
    setCompose(true);
  }
  useEffect(() => {
    if (!user) return;
    const key = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement;
      if (
        e.key.toLowerCase() === "n" &&
        !e.ctrlKey &&
        !e.metaKey &&
        !e.altKey &&
        !compose &&
        !target.closest("input,textarea,[contenteditable=true]")
      ) {
        e.preventDefault();
        setCompose(true);
      }
    };
    document.addEventListener("keydown", key);
    return () => document.removeEventListener("keydown", key);
  }, [user, compose]);
  return (
    <>
      {user ? (
        <>
          <Workspace
            user={user}
            onLogout={logout}
            onWrite={write}
            refreshVersion={refresh}
            notify={notify}
          />
          <Suspense
            fallback={
              compose ? (
                <div className="toast" role="status">
                  正在加载写信窗口…
                </div>
              ) : null
            }
          >
            <Compose
              open={compose}
              onOpenChange={setCompose}
              user={user}
              draft={draft}
              onDraft={setDraft}
              onSent={() => {
                setRefresh((value) => value + 1);
                notify("邮件已提交，副本已保存到「已发送」。");
              }}
            />
          </Suspense>
        </>
      ) : (
        <Login busy={checking} notice={notice} onLogin={login} />
      )}
      {toast && (
        <div className="toast" role="status">
          <span>{toast}</span>
          <button aria-label="关闭提示" onClick={() => setToast("")}>
            <X size={15} />
          </button>
        </div>
      )}
    </>
  );
}
