import { useState, useEffect, useRef, type FormEvent } from "react";
import * as Dialog from "@radix-ui/react-dialog";
import { EditorContent, useEditor, useEditorState } from "@tiptap/react";
import StarterKit from "@tiptap/starter-kit";
import Placeholder from "@tiptap/extension-placeholder";
import {
  X,
  Send,
  Bold,
  Italic,
  List,
  Quote,
  RotateCcw,
  AlertCircle,
} from "lucide-react";
import {
  ApiError,
  blankDraft,
  errorText,
  request,
  splitAddresses,
  type Draft,
  type User,
} from "./api";

interface Props {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  user: User;
  draft: Draft;
  onDraft: (draft: Draft) => void;
  onSent: () => void;
}
export default function Compose({
  open,
  onOpenChange,
  user,
  draft,
  onDraft,
  onSent,
}: Props) {
  const [sending, setSending] = useState(false);
  const [error, setError] = useState("");
  const uncertain = draft.pendingVerification || "";
  const active = useRef(true);
  useEffect(() => {
    active.current = true;
    return () => {
      active.current = false;
    };
  }, []);
  const latest = useRef({ draft, onDraft });
  latest.current = { draft, onDraft };
  const setUncertain = (id: string) =>
    onDraft({ ...latest.current.draft, pendingVerification: id });
  const editor = useEditor(
    {
      extensions: [
        StarterKit,
        Placeholder.configure({ placeholder: "在这里写下你想说的…" }),
      ],
      content: draft.html,
      immediatelyRender: false,
      editorProps: {
        attributes: {
          "aria-label": "邮件正文",
          role: "textbox",
          "aria-multiline": "true",
        },
      },
      onUpdate: ({ editor }) =>
        latest.current.onDraft({
          ...latest.current.draft,
          html: editor.getHTML(),
          text: editor.getText(),
        }),
    },
    [user.address],
  );
  const format = useEditorState({
    editor,
    selector: ({ editor }) => ({
      bold: editor?.isActive("bold") ?? false,
      italic: editor?.isActive("italic") ?? false,
      list: editor?.isActive("bulletList") ?? false,
      quote: editor?.isActive("blockquote") ?? false,
    }),
  });
  useEffect(() => {
    if (editor && editor.getHTML() !== draft.html && !editor.isFocused)
      editor.commands.setContent(draft.html || "", { emitUpdate: false });
  }, [editor, draft.html]);
  useEffect(() => {
    editor?.setEditable(!sending);
  }, [editor, sending]);
  async function submit(event: FormEvent) {
    event.preventDefault();
    if (sending || uncertain) return;
    const to = splitAddresses(draft.to),
      cc = splitAddresses(draft.cc);
    if (!to.length && !cc.length) {
      setError("请至少填写一位收件人。");
      return;
    }
    if (
      to.length + cc.length > 100 ||
      [...to, ...cc].some((x) => !/^[^\s@<>]+@[^\s@<>]+\.[^\s@<>]+$/.test(x))
    ) {
      setError("请检查收件地址，多个地址之间使用逗号分隔。");
      return;
    }
    setSending(true);
    setError("");
    // Keep the resend guard in App's draft if a session change unmounts this form.
    setUncertain("当前提交尚未确认");
    try {
      await request("/api/send", {
        method: "POST",
        body: JSON.stringify({
          to,
          cc,
          subject: draft.subject,
          text: editor?.getText() ?? draft.text,
          html: editor?.getHTML() ?? draft.html,
        }),
      });
      if (!active.current) return;
      onDraft(blankDraft());
      editor?.commands.setContent("", { emitUpdate: false });
      onOpenChange(false);
      onSent();
    } catch (err) {
      if (!active.current) return;
      if (err instanceof ApiError && (err.status === 504 || err.status === 0))
        setUncertain(err.payload.message_id || "本次提交");
      else {
        setUncertain("");
        setError(errorText(err));
      }
    } finally {
      if (active.current) setSending(false);
    }
  }
  function discard() {
    onDraft(blankDraft());
    editor?.commands.setContent("", { emitUpdate: false });
    setError("");
    onOpenChange(false);
  }
  return (
    <Dialog.Root
      open={open}
      onOpenChange={(value) => {
        if (!sending) onOpenChange(value);
      }}
    >
      <Dialog.Portal>
        <Dialog.Overlay className="dialog-overlay" />
        <Dialog.Content
          className="compose-dialog"
          onInteractOutside={(event) => event.preventDefault()}
        >
          <div className="compose-heading">
            <div>
              <span className="eyebrow">NEW MESSAGE</span>
              <Dialog.Title>写一封新邮件</Dialog.Title>
            </div>
            <Dialog.Close
              className="icon-button"
              aria-label="收起写信"
              disabled={sending}
            >
              <X size={19} />
            </Dialog.Close>
          </div>
          <Dialog.Description className="compose-description">
            收起后内容保留在当前页面，刷新页面会丢失。
          </Dialog.Description>
          <form onSubmit={submit}>
            <fieldset disabled={sending}>
              <div className="compose-field">
                <span>发件人</span>
                <span className="compose-from">{user.address}</span>
              </div>
              <label className="compose-field">
                <span>收件人</span>
                <input
                  aria-label="收件人"
                  value={draft.to}
                  onChange={(e) => onDraft({ ...draft, to: e.target.value })}
                  placeholder="name@example.com，逗号分隔多个地址"
                  autoFocus
                />
              </label>
              <label className="compose-field">
                <span>抄送</span>
                <input
                  aria-label="抄送"
                  value={draft.cc}
                  onChange={(e) => onDraft({ ...draft, cc: e.target.value })}
                  placeholder="可选"
                />
              </label>
              <label className="compose-field subject-field">
                <span>主题</span>
                <input
                  aria-label="主题"
                  value={draft.subject}
                  onChange={(e) =>
                    onDraft({ ...draft, subject: e.target.value })
                  }
                  placeholder="给邮件起一个主题"
                />
              </label>
            </fieldset>
            <EditorContent editor={editor} className="compose-editor" />
            <div
              className="editor-toolbar"
              role="toolbar"
              aria-label="正文格式"
            >
              <button
                type="button"
                aria-label="加粗"
                disabled={sending}
                aria-pressed={format?.bold ?? false}
                onClick={() => editor?.chain().focus().toggleBold().run()}
              >
                <Bold size={16} />
              </button>
              <button
                type="button"
                aria-label="斜体"
                aria-pressed={format?.italic ?? false}
                disabled={sending}
                onClick={() => editor?.chain().focus().toggleItalic().run()}
              >
                <Italic size={16} />
              </button>
              <span className="toolbar-divider" />
              <button
                type="button"
                aria-label="项目列表"
                aria-pressed={format?.list ?? false}
                disabled={sending}
                onClick={() => editor?.chain().focus().toggleBulletList().run()}
              >
                <List size={17} />
              </button>
              <button
                type="button"
                aria-label="引用"
                aria-pressed={format?.quote ?? false}
                disabled={sending}
                onClick={() => editor?.chain().focus().toggleBlockquote().run()}
              >
                <Quote size={16} />
              </button>
            </div>
            {error && (
              <div className="inline-error" role="alert">
                <AlertCircle size={16} />
                {error}
              </div>
            )}
            {uncertain && !sending && (
              <div className="uncertain-banner" role="alert">
                <strong>提交结果待确认</strong>
                <p>
                  邮件可能已提交，请先核实已发送邮箱和收件情况。系统不会自动重发。
                </p>
                <code>{uncertain}</code>
                <button type="button" onClick={() => setUncertain("")}>
                  我已核实，允许再次发送
                </button>
              </div>
            )}
            <div className="compose-footer">
              <button
                className="primary-button"
                type="submit"
                disabled={sending || !!uncertain}
              >
                <Send size={16} />
                {sending ? "正在提交…" : "发送邮件"}
              </button>
              <span>提交成功后保存在「已发送」</span>
              <button
                className="text-button discard"
                type="button"
                disabled={sending}
                onClick={discard}
              >
                <RotateCcw size={14} />
                放弃这封邮件
              </button>
            </div>
          </form>
        </Dialog.Content>
      </Dialog.Portal>
    </Dialog.Root>
  );
}
