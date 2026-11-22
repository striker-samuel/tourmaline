// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/RHI/upload_ticket.h>
#include <cstdio>
using namespace smvk;
static int fails=0;
#define CHECK(x) do { if(!(x)){++fails;std::fprintf(stderr,"ticket test failure: %s:%d\\n",__FILE__,__LINE__);} }while(0)
int main() {
    UploadSequence s;
    CHECK(s.Poll(0,0)==UploadStatus::Invalid);
    CHECK(s.Poll(1,0)==UploadStatus::Invalid);
    const auto a=s.Issue();
    const auto b=s.Issue();
    CHECK(a==1 && b==2);
    CHECK(s.Poll(a,0)==UploadStatus::Recorded);
    s.Submitted(a,5);
    CHECK(s.Poll(a,4)==UploadStatus::Submitted);
    CHECK(s.Poll(a,5)==UploadStatus::Completed);
    CHECK(s.Poll(b,5)==UploadStatus::Recorded);
    s.Submitted(b,8);
    CHECK(s.Poll(a,5)==UploadStatus::Submitted); // conservative latest batch
    CHECK(s.Poll(a,8)==UploadStatus::Completed);
    CHECK(s.Poll(b,8)==UploadStatus::Completed);
    const auto c=s.Issue();
    s.Failed(c);
    CHECK(s.Poll(c,UINT64_MAX)==UploadStatus::Failed);
    CHECK(s.Poll(b,8)==UploadStatus::Completed);
    std::printf("upload ticket tests: %d failures\\n",fails);
    return fails?1:0;
}
