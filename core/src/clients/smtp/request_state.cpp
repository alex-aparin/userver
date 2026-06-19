#include <clients/smtp/request_state.hpp>
#include <userver/utils/overloaded.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::smtp {


RequestState::RequestState(
    common::impl::EasyWrapper&& wrapper,
    common::RequestStats&& req_stats,
    const std::shared_ptr<common::DestinationStatistics>& dest_stats,
    clients::dns::Resolver* resolver,
    const tracing::TracingManagerBase& tracing_manager
)
    : common::RequestState(std::move(wrapper), std::move(req_stats), dest_stats, resolver, tracing_manager)
{
    RequestCompleted();
}

RequestState::~RequestState() {
}

void RequestState::SetError(std::error_code& err) {
    const utils::Overloaded visitor{
        [this, &err](EmailFullBufferedData& buffered_data) {
            {
                [[maybe_unused]] const auto cleanup = this->response_move();
            }
            auto promise = std::move(buffered_data.promise);
            // The task will wake up and may reuse RequestState.
            promise.set_exception(this->PrepareException(err));
        }
    };
    std::visit(visitor, data_);
}

void RequestState::SetResponse(long raw_status_code, tracing::Span& span) {
    response()->SetStatusCode(static_cast<smtp::Status>(raw_status_code));
    response()->SetStats(easy().get_local_stats());

    if (response()->IsError()) {
        span.AddTag(tracing::kErrorFlag, true);
    }

    const utils::Overloaded visitor{
            [this](EmailFullBufferedData& buffered_data) {
                auto promise = std::move(buffered_data.promise);
                // The task will wake up and may reuse RequestState.
                promise.set_value(this->response_move());
            }
        };
        std::visit(visitor, data_);
}

void RequestState::SetException(std::exception_ptr& exc) {
    const utils::Overloaded visitor{
        [&exc](EmailFullBufferedData& buffered_data) {
            auto promise = std::move(buffered_data.promise);
            // The task will wake up and may reuse RequestState.
            promise.set_exception(std::move(exc));
        }
    };
    std::visit(visitor, data_);
}


void RequestState::SetCurrentException() {
    auto exception_handler = utils::Overloaded{
        [](EmailFullBufferedData& data) { data.promise.set_exception(std::current_exception()); },
    };
    std::visit(exception_handler, data_);
}

long RequestState::PreCheckErr(std::error_code& err, tracing::Span& span) {
    auto& easy = this->easy();
    if (response()->status_code() == smtp::Status::kInvalid && !err) {
        // We haven't received the full set of headers, the response is truncated
        err = std::error_code(curl::errc::EasyErrorCode::kRecvError);

        response()->SetStatusCode(smtp::Status::kInternalServerError);
    }

    const auto raw_status_code = easy.get_response_code();
    const auto status_code = static_cast<http::Status>(raw_status_code);

    CheckResponseDeadline(err, status_code);

    //if (holder->testsuite_config_ && !err) {
    //    const auto& headers = holder->response()->headers();
    //    err = TestsuiteResponseHook(status_code, headers, span);
    //}

    return raw_status_code;
}

long RequestState::GetResponseRawStatusCode() {
    return response()->status_code();
}
void RequestState::ResetResponse() {
    response_ = std::make_shared<smtp::Response>();
    response_->SetStatusCode(smtp::Status::kInvalid);
}
void RequestState::ClearResponseBody() {
    UASSERT(response_);
    response_->sink_string().clear();
    response_->body().clear();
}

bool RequestState::IsDeadlineExpiredResponse(long raw_status_code) {

    return false;
}

engine::Future<std::shared_ptr<smtp::Response>> RequestState::async_perform_smtp(
         utils::impl::SourceLocation location
    ) {
    WaitForRequestCompletion();
    auto& data = data_.emplace<EmailFullBufferedData>();

    StartNewSpan(location);
    ResetDataForNewRequest();

    auto& span = span_storage_->Get();
    span.AddTag("stream_api", 1);

    easy().set_read_function(&RequestState::MessageBodyReadFunction);
    easy().set_read_data(this);
    easy().set_upload(true);
    // Force no retries
    retry_.retries = 1;

    auto future = data.promise.get_future();

    if (UpdateTimeoutFromDeadlineAndCheck()) {
        PerformRequest([holder = shared_from_this()](std::error_code err) mutable {
            RequestState::OnCompleted(std::move(holder), err);
        });
    }

    return future;
}

}  // namespace clients::http

USERVER_NAMESPACE_END
