#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <grpcpp/grpcpp.h>

#include "activity.h"
#include "dedup_index.h"
#include "leaky_bucket_queue.h"
#include "media_upload.grpc.pb.h"
#include "upload_job.h"

// The producer side of the queue. gRPC runs each upload on its own thread, so
// any number of producers can be sending at the same time.
class UploadService final : public mediaupload::MediaUpload::Service {
public:
    UploadService(LeakyBucketQueue<UploadJob>& queue, DedupIndex& dedup, Activity& activity,
                  std::filesystem::path tempDir, std::uint64_t maxUploadBytes);

    grpc::Status Upload(grpc::ServerContext* context,
                        grpc::ServerReaderWriter<mediaupload::UploadReply, mediaupload::UploadMessage>* stream) override;

private:
    // the names and temp path are filled in as they become known, so Upload can report a failure against them
    grpc::Status receive(grpc::ServerContext* context,
                         grpc::ServerReaderWriter<mediaupload::UploadReply, mediaupload::UploadMessage>* stream,
                         std::string& producerId, std::string& fileName, std::filesystem::path& tempPath);

    LeakyBucketQueue<UploadJob>& queue_;
    DedupIndex& dedup_;
    Activity& activity_;
    std::filesystem::path tempDir_;
    std::uint64_t maxUploadBytes_;
};
