/*******************************************************************************
* Copyright 2019 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "sycl/sycl_stream_cpu_thunk.hpp"

#include "common/c_types_map.hpp"
#include "common/engine.hpp"
#include "common/utils.hpp"
#include "common/primitive.hpp"
#include "sycl/sycl_stream.hpp"

#include <assert.h>

using namespace mkldnn::impl;
using namespace mkldnn::impl::sycl;

extern "C" void mkldnn_impl_sycl_cpu_thunk(const thunk_params_t *params) {

    auto *submit_ctx = reinterpret_cast<submit_ctx_t *>(params->submit_ctx_ptr);
    auto *prim = submit_ctx->prim;

    assert(params->size == submit_ctx->sycl_mem_storages.size());
    for (int i = 0; i < params->size; i++) {
        auto *mem_storage = submit_ctx->sycl_mem_storages[i];
        void *ptr = reinterpret_cast<void *>(params->native_pointers[i]);
        submit_ctx->exec_ctx.register_memory_storage_mapping(mem_storage, ptr);
    }

    prim->execute(submit_ctx->exec_ctx);

    const_cast<primitive_t *>(prim)->release();

    delete submit_ctx;
}
