// 2026-10-18: session loss during submission must preserve the resend guard;
// editor format state must follow transactions. See docs/bugfixes/2026-10-18-webmail-browser-contract.md.
import { test, expect, type Page, type Route } from "@playwright/test";
import { readFileSync, mkdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { resolve } from "node:path";
const fixture = JSON.parse(
  readFileSync(process.env.WEBMAIL_E2E_STATE!, "utf8"),
);
const screenshots = resolve("../artifacts/webmail-preview");
mkdirSync(screenshots, { recursive: true });
const subject = "设计评审｜新版邮箱，一起让沟通更简单";
const resource = `/api/mailboxes/${fixture.boxId}/messages/${fixture.mailId}`;
async function login(page: Page) {
  await page.getByRole("textbox", { name: "邮箱地址" }).fill(fixture.address);
  await page.getByLabel("密码", { exact: true }).fill(fixture.password);
  await page.getByRole("button", { name: "登录邮箱", exact: true }).click();
  await expect(page.getByRole("heading", { name: "收件箱" })).toBeVisible();
  await expect(page.getByLabel("正在加载邮件")).toHaveCount(0);
}
async function openCompose(page: Page) {
  await page.getByRole("button", { name: "写邮件", exact: false }).click();
  await expect(page.getByRole("dialog")).toBeVisible();
}
async function fillDraft(page: Page) {
  await page
    .getByRole("textbox", { name: "收件人", exact: true })
    .fill(fixture.address);
  await page
    .getByRole("textbox", { name: "抄送", exact: true })
    .fill(fixture.cc);
  await page
    .getByRole("textbox", { name: "主题", exact: true })
    .fill("来自浏览器的问候");
  await page
    .getByRole("textbox", { name: "邮件正文", exact: true })
    .fill("你好，这是从真实浏览器发送的邮件。");
}

test("real inbox: lossless IDs, paging, MIME sandbox, owner-scoped flags and bounded binary download", async ({
  page,
  playwright,
}) => {
  const errors: string[] = [];
  const external: string[] = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("request", (req) => {
    if (req.url().startsWith("http") && !req.url().startsWith(fixture.baseURL))
      external.push(req.url());
  });
  await page.goto("/");
  await expect(
    page.getByRole("button", { name: "登录邮箱", exact: true }),
  ).toBeEnabled();
  await page.screenshot({
    path: resolve(screenshots, "login.png"),
    fullPage: true,
  });
  await page.getByRole("textbox", { name: "邮箱地址" }).fill(fixture.address);
  await page.getByLabel("密码", { exact: true }).fill("incorrect-password");
  await page.getByRole("button", { name: "登录邮箱", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("邮箱或密码不正确");
  await login(page);
  await page.screenshot({
    path: resolve(screenshots, "inbox.png"),
    fullPage: true,
  });
  await expect(
    page.getByRole("button", { name: "下一页", exact: true }),
  ).toBeEnabled();
  await page.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(page.locator(".list-pagination")).toContainText("第 2 / 2 页");
  await expect(page.locator(".mail-items > li")).toHaveCount(4);
  await page.getByRole("button", { name: "上一页", exact: true }).click();
  await page
    .getByRole("button", { name: `打开邮件：${subject}`, exact: true })
    .click();
  await expect(page.locator(".mail-detail h2")).toHaveText(subject);
  await expect(
    page.getByRole("button", { name: "标记为未读", exact: true }),
  ).toBeVisible();
  await expect(
    page.frameLocator("iframe").getByText("本次评审主要讨论："),
  ).toBeVisible();
  expect(await page.locator("iframe").getAttribute("sandbox")).toBe("");
  expect(await page.evaluate(() => "__mailScriptExecuted" in window)).toBe(
    false,
  );
  const detail = await (await page.request.get(resource)).json();
  expect(detail.message.id).toBe(fixture.mailId);
  expect(detail.message.status).toBe(0);
  await page.screenshot({
    path: resolve(screenshots, "reading.png"),
    fullPage: true,
  });
  await page.getByRole("button", { name: "标星当前邮件", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "取消当前邮件标星", exact: true }),
  ).toBeVisible();
  expect(
    (await (await page.request.get(resource)).json()).message.is_starred,
  ).toBe(true);
  const downloadPath = `${resource}/attachments/${fixture.attachmentId}`;
  const head = await page.request.head(downloadPath);
  expect(head.status()).toBe(200);
  expect(Number(head.headers()["content-length"])).toBe(fixture.attachmentSize);
  expect((await head.body()).length).toBe(0);
  const downloaded = page.waitForEvent("download");
  await page.getByRole("button", { name: "下载附件：项目资料.bin" }).click();
  const download = await downloaded;
  expect(download.suggestedFilename()).toBe("项目资料.bin");
  const file = readFileSync((await download.path())!);
  expect(file.length).toBe(fixture.attachmentSize);
  expect(createHash("sha256").update(file).digest("hex")).toBe(
    fixture.attachmentHash,
  );
  expect(
    (
      await page.request.patch(
        `/api/mailboxes/${fixture.ccBoxId}/messages/${fixture.mailId}`,
        { data: { read: true } },
      )
    ).status(),
  ).toBe(404);
  expect(
    (
      await page.request.get(
        `/api/mailboxes/${fixture.ccBoxId}/messages/${fixture.mailId}/attachments/${fixture.attachmentId}`,
      )
    ).status(),
  ).toBe(404);
  const cc = await playwright.request.newContext({ baseURL: fixture.baseURL });
  await cc.post("/api/login", {
    data: { address: fixture.cc, password: fixture.password },
  });
  const ccDetail = await (
    await cc.get(`/api/mailboxes/${fixture.ccBoxId}/messages/${fixture.mailId}`)
  ).json();
  expect(ccDetail.message.status).toBe(1);
  expect(ccDetail.message.is_starred).toBe(false);
  await cc.dispose();
  await page.getByRole("button", { name: "标记为未读", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "标记为已读", exact: true }),
  ).toBeVisible();
  expect((await (await page.request.get(resource)).json()).message.status).toBe(
    1,
  );
  expect(errors).toEqual([]);
  expect(external).toEqual([]);
});

test("compose sends through the real persistence service, atomic Sent copy and fresh empty draft", async ({
  page,
}) => {
  await page.goto("/");
  await login(page);
  await openCompose(page);
  await page.getByRole("button", { name: "加粗", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "加粗", exact: true }),
  ).toHaveAttribute("aria-pressed", "true");
  await fillDraft(page);
  await page.screenshot({
    path: resolve(screenshots, "compose.png"),
    fullPage: true,
  });
  const accepted = page.waitForResponse((response) =>
    response.url().endsWith("/api/send"),
  );
  await page.getByRole("button", { name: "发送邮件", exact: true }).click();
  expect((await accepted).status()).toBe(202);
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await expect(page.getByRole("status")).toContainText("副本已保存");
  const boxes = (await (await page.request.get("/api/mailboxes")).json())
    .mailboxes;
  const sentBox = boxes.find((b: { box_type: number }) => b.box_type === 2);
  expect(sentBox.total).toBe(1);
  expect(sentBox.unread).toBe(0);
  await page.getByRole("button", { name: "已发送", exact: true }).click();
  await page
    .getByRole("button", { name: "打开邮件：来自浏览器的问候", exact: true })
    .click();
  await expect(page.locator(".mail-detail h2")).toHaveText("来自浏览器的问候");
  const sent = await (
    await page.request.get(`/api/mailboxes/${sentBox.id}/messages`)
  ).json();
  const mid = sent.messages[0].id;
  const ownInbox = await (
    await page.request.get(`/api/mailboxes/${fixture.boxId}/messages/${mid}`)
  ).json();
  expect(ownInbox.message.status).toBe(1); // Reading Sent must not mark the recipient's Inbox read, even for self-send.
  expect(ownInbox.message.recipients.sort()).toEqual(
    [fixture.address, fixture.cc].sort(),
  );
  expect(ownInbox.body.text).toContain("真实浏览器发送的邮件");
  expect(ownInbox.body.html).toContain("<strong>");
  await openCompose(page);
  await expect(
    page.getByRole("textbox", { name: "收件人", exact: true }),
  ).toHaveValue("");
  await expect(
    page.getByRole("textbox", { name: "主题", exact: true }),
  ).toHaveValue("");
  await expect(
    page.getByRole("textbox", { name: "邮件正文", exact: true }),
  ).toHaveText("");
  await page.getByRole("button", { name: "放弃这封邮件", exact: true }).click();
});

test("new login revokes this browser, preserves its in-memory draft and explicit logout clears it", async ({
  page,
  playwright,
}) => {
  await page.goto("/");
  await login(page);
  await openCompose(page);
  await fillDraft(page);
  await page.getByRole("button", { name: "收起写信", exact: true }).click();
  const other = await playwright.request.newContext({
    baseURL: fixture.baseURL,
  });
  expect(
    (
      await other.post("/api/login", {
        data: { address: fixture.address, password: fixture.password },
      })
    ).status(),
  ).toBe(200);
  await page.getByRole("button", { name: "刷新邮件", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("登录已失效");
  await login(page);
  expect((await other.get("/api/me")).status()).toBe(401);
  await other.dispose();
  await openCompose(page);
  await expect(
    page.getByRole("textbox", { name: "主题", exact: true }),
  ).toHaveValue("来自浏览器的问候");
  await expect(
    page.getByRole("textbox", { name: "邮件正文", exact: true }),
  ).toContainText("真实浏览器发送的邮件");
  await page.getByRole("button", { name: "收起写信", exact: true }).click();
  await page.getByRole("button", { name: "退出登录", exact: true }).click();
  await login(page);
  await openCompose(page);
  await expect(
    page.getByRole("textbox", { name: "主题", exact: true }),
  ).toHaveValue("");
});

test("session loss during an in-flight submission keeps the resend guard and ignores late form callbacks", async ({
  page,
  playwright,
}) => {
  await page.goto("/");
  await login(page);
  await openCompose(page);
  await fillDraft(page);
  let pending: Route | undefined;
  await page.route("**/api/send", (route) => {
    pending = route;
  });
  await page.getByRole("button", { name: "发送邮件", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "正在提交…", exact: true }),
  ).toBeVisible();
  const replacement = await playwright.request.newContext({
    baseURL: fixture.baseURL,
  });
  await replacement.post("/api/login", {
    data: { address: fixture.address, password: fixture.password },
  });
  // Returning focus to the tab refreshes the real mailbox API and observes the revoked cookie.
  await page.evaluate(() => window.dispatchEvent(new Event("focus")));
  await expect(page.getByRole("status")).toContainText("登录已失效");
  await login(page);
  await openCompose(page);
  await expect(
    page.getByRole("button", { name: "发送邮件", exact: true }),
  ).toBeDisabled();
  await pending!.fulfill({
    status: 202,
    contentType: "application/json",
    body: '{"status":"accepted"}',
  });
  await expect(page.getByRole("dialog")).toBeVisible();
  await expect(
    page.getByRole("textbox", { name: "主题", exact: true }),
  ).toHaveValue("来自浏览器的问候");
  await expect(page.getByRole("alert")).toContainText("提交结果待确认");
  await replacement.dispose();
});

for (const failure of ["timeout", "network"] as const) {
  test(`unknown send result (${failure}) never retries and blocks resending across dialog reopen`, async ({
    page,
  }) => {
    await page.goto("/");
    await login(page);
    await openCompose(page);
    await fillDraft(page);
    let calls = 0;
    await page.route("**/api/send", async (route) => {
      ++calls;
      if (failure === "network") await route.abort("connectionfailed");
      else
        await route.fulfill({
          status: 504,
          contentType: "application/json",
          body: JSON.stringify({
            message_id: "<uncertain@webmail.test>",
            error: "timeout",
            retryable: false,
          }),
        });
    });
    await page.getByRole("button", { name: "发送邮件", exact: true }).click();
    await expect(page.getByRole("alert")).toContainText("提交结果待确认");
    await expect(
      page.getByRole("button", { name: "发送邮件", exact: true }),
    ).toBeDisabled();
    await page.getByRole("button", { name: "收起写信", exact: true }).click();
    await openCompose(page);
    await expect(
      page.getByRole("button", { name: "发送邮件", exact: true }),
    ).toBeDisabled();
    expect(calls).toBe(1);
    await page
      .getByRole("button", { name: "我已核实，允许再次发送", exact: true })
      .click();
    await expect(
      page.getByRole("button", { name: "发送邮件", exact: true }),
    ).toBeEnabled();
    expect(calls).toBe(1);
  });
}

test("mobile navigation, reading and compose fit the viewport; failed fetch has a usable retry", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("/");
  await login(page);
  await page.route("**/messages?*", (route) =>
    route.fulfill({
      status: 503,
      contentType: "application/json",
      body: '{"error":"unavailable"}',
    }),
  );
  await page.getByRole("button", { name: "刷新邮件", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("暂时无法加载邮件");
  await page.unroute("**/messages?*");
  await page.getByRole("button", { name: "重新加载", exact: true }).click();
  await expect(page.getByLabel("正在加载邮件")).toHaveCount(0);
  await page
    .getByRole("button", { name: `打开邮件：${subject}`, exact: true })
    .click();
  await expect(page.locator(".mail-detail h2")).toHaveText(subject);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
  await page.screenshot({
    path: resolve(screenshots, "mobile-reading.png"),
    fullPage: true,
  });
  await page.getByRole("button", { name: "返回邮件列表", exact: true }).click();
  await page.getByRole("button", { name: "打开邮箱导航", exact: true }).click();
  await openCompose(page);
  await fillDraft(page);
  const dialog = await page.getByRole("dialog").boundingBox();
  expect(dialog!.width).toBeLessThanOrEqual(390);
  await page.screenshot({
    path: resolve(screenshots, "mobile-compose.png"),
    fullPage: true,
  });
});
