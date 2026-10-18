#pragma once
#include "mail_system/back/webmail/smtp_submitter.h"
#include "mail_system/back/submission/mail_submission_service.h"
namespace mail_system::webmail {
class DirectSubmitter final : public ISmtpSubmitter {
  public:
    explicit DirectSubmitter(std::shared_ptr<submission::MailSubmissionService> service)
        : service_(std::move(service)) {}
    void submit_async(SubmitRequest request, Complete complete) override;

  private:
    std::shared_ptr<submission::MailSubmissionService> service_;
};
} // namespace mail_system::webmail
