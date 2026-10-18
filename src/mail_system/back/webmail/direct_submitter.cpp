#include "mail_system/back/webmail/direct_submitter.h"
namespace mail_system::webmail {
void DirectSubmitter::submit_async(SubmitRequest request, Complete complete) {
    submission::Request submission;
    submission.user_id = request.user_id;
    submission.sender = std::move(request.sender);
    submission.subject = std::move(request.subject);
    submission.message_id = std::move(request.message_id);
    submission.raw = std::move(request.raw);
    submission.recipients = std::move(request.recipients);
    service_->submit_async(std::move(submission), [complete = std::move(complete)](submission::Result r) {
        SubmitStatus status = SubmitStatus::Unavailable;
        switch (r.status) {
        case submission::Status::Accepted:
            status = SubmitStatus::Accepted;
            break;
        case submission::Status::AuthFailed:
            status = SubmitStatus::AuthFailed;
            break;
        case submission::Status::Rejected:
            status = SubmitStatus::Rejected;
            break;
        case submission::Status::Unknown:
            status = SubmitStatus::OutcomeUnknown;
            break;
        case submission::Status::Unavailable:
            break;
        }
        complete({status, r.code, std::move(r.stage)});
    });
}
} // namespace mail_system::webmail
