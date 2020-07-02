// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "pipelinecache.h"

#include "gpu.h"

namespace ncnn {

#if NCNN_VULKAN
// https://en.wikipedia.org/wiki/MurmurHash
static uint32_t murmur3_32(const uint32_t* data, int size)
{
    uint32_t h = 0;

    for (int i = 0; i < size; i++)
    {
        uint32_t k = *data++;

        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> (32 - 15));
        k *= 0x1b873593;

        h ^= k;
        h = (h << 13) | (h >> (32 - 13));
        h = (h * 5) + 0xe6546b64;
    }

    h ^= size * 4;

    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;

    return h;
}

// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1a_hash
static uint32_t fnv1a_32(const uint8_t* data, int size)
{
    uint32_t h = 0x811c9dc5;

    for (int i = 0; i < size; i++)
    {
        h ^= (uint32_t)*data++;
        h *= 0x01000193;
    }

    return h;
}

PipelineCache::pipeline_cache_digest::pipeline_cache_digest(int _shader_type_index, const Option& opt, const std::vector<vk_specialization_type>& specializations,
                                                            uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z)
{
    shader_type_index = _shader_type_index;

    // encode opt
    opt_local_size_bits[0] = opt.use_image_storage << 7
        | opt.use_fp16_packed << 6
        | opt.use_fp16_storage << 5
        | opt.use_fp16_arithmetic << 4
        | opt.use_int8_storage << 3
        | opt.use_int8_arithmetic << 2;

    // encode local_size
    opt_local_size_bits[1] = local_size_x;
    opt_local_size_bits[2] = local_size_y;
    opt_local_size_bits[3] = local_size_z;

    // encode specializations
    const int specialization_count = specializations.size();
    specializations_murmur3 = murmur3_32((const uint32_t*)specializations.data(), specialization_count);
    specializations_fnv1a = fnv1a_32((const uint8_t*)specializations.data(), specialization_count * sizeof(vk_specialization_type));
}

PipelineCache::PipelineCache(const VulkanDevice* _vkdev)
{
    vkdev = _vkdev;
}

PipelineCache::~PipelineCache()
{
    clear();
}

void PipelineCache::clear()
{
    for (size_t i = 0; i < cache_artifacts.size(); i++)
    {
        const pipeline_cache_artifact& cc = cache_artifacts[i];

        if (vkdev->info.support_VK_KHR_descriptor_update_template)
        {
            if (cc.descriptor_update_template)
            {
                vkdev->vkDestroyDescriptorUpdateTemplateKHR(vkdev->vkdevice(), cc.descriptor_update_template, 0);
            }
        }

        if (cc.pipeline)
        {
            vkDestroyPipeline(vkdev->vkdevice(), cc.pipeline, 0);
        }

        if (cc.pipeline_layout)
        {
            vkDestroyPipelineLayout(vkdev->vkdevice(), cc.pipeline_layout, 0);
        }

        if (cc.descriptorset_layout)
        {
            vkDestroyDescriptorSetLayout(vkdev->vkdevice(), cc.descriptorset_layout, 0);
        }

        if (cc.shader_module)
        {
            vkDestroyShaderModule(vkdev->vkdevice(), cc.shader_module, 0);
        }
    }

    cache_digests.clear();
    cache_artifacts.clear();
}

int PipelineCache::get_pipeline(const uint32_t* spv_data, size_t spv_data_size, const std::vector<vk_specialization_type>& specializations,
                                uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z,
                                VkShaderModule* _shader_module,
                                VkDescriptorSetLayout* _descriptorset_layout,
                                VkPipelineLayout* _pipeline_layout,
                                VkPipeline* _pipeline,
                                VkDescriptorUpdateTemplateKHR* _descriptor_update_template,
                                ShaderInfo& shader_info)
{
    // TODO mutex lock

    int ret = 0;

    // find cache


    ret = resolve_shader_info(spv_data, spv_data_size, shader_info);
    if (ret != 0)
    {
        NCNN_LOGE("resolve_shader_info failed %d", ret);
        return -1;
    }

    VkShaderModule shader_module = 0;
//     if (vkdev->info.bug_local_size_spec_const)
//     {
        shader_module = vkdev->compile_shader_module(spv_data, spv_data_size, local_size_x, local_size_y, local_size_z);
//     }
//     else
//     {
//         shader_module = vkdev->compile_shader_module(spv_data, spv_data_size);
//     }

    if (!shader_module)
    {
        NCNN_LOGE("create_shader_module failed");
        return -1;
    }

    ret = new_pipeline(shader_module, shader_info, specializations, _descriptorset_layout, _pipeline_layout, _pipeline, _descriptor_update_template);
    if (ret != 0)
    {
        vkDestroyShaderModule(vkdev->vkdevice(), shader_module, 0);
        return -1;
    }

    // TODO save to cache

    *_shader_module = shader_module;

    return 0;
}

int PipelineCache::get_pipeline(int shader_type_index, const Option& opt, const std::vector<vk_specialization_type>& specializations,
                                uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z,
                                VkShaderModule* _shader_module,
                                VkDescriptorSetLayout* descriptorset_layout,
                                VkPipelineLayout* pipeline_layout,
                                VkPipeline* pipeline,
                                VkDescriptorUpdateTemplateKHR* descriptor_update_template,
                                ShaderInfo& shader_info)
{
    // find cache
    pipeline_cache_digest key(shader_type_index, opt, specializations, local_size_x, local_size_y, local_size_z);
    for (size_t i = 0; i < cache_digests.size(); i++)
    {
        if (cache_digests[i] == key)
        {
            // hit cache
            const pipeline_cache_artifact& cc = cache_artifacts[i];

            *_shader_module = cc.shader_module;
            *descriptorset_layout = cc.descriptorset_layout;
            *pipeline_layout = cc.pipeline_layout;
            *pipeline = cc.pipeline;
            *descriptor_update_template = cc.descriptor_update_template;
            shader_info = cc.shader_info;

            return 0;
        }
    }

    int ret = 0;

    // create new pipeline
    VkShaderModule shader_module = 0;
    ret = create_shader_module(shader_type_index, opt, local_size_x, local_size_y, local_size_z, &shader_module, shader_info);
    if (ret != 0)
    {
        return -1;
    }

    ret = new_pipeline(shader_module, shader_info, specializations, descriptorset_layout, pipeline_layout, pipeline, descriptor_update_template);
    if (ret != 0)
    {
        vkDestroyShaderModule(vkdev->vkdevice(), shader_module, 0);
        return -1;
    }

    *_shader_module = shader_module;

    // save to cache
    {
        pipeline_cache_artifact cc;

        cc.shader_module = *_shader_module;
        cc.descriptorset_layout = *descriptorset_layout;
        cc.pipeline_layout = *pipeline_layout;
        cc.pipeline = *pipeline;
        cc.descriptor_update_template = *descriptor_update_template;
        cc.shader_info = shader_info;

        cache_digests.push_back(key);
        cache_artifacts.push_back(cc);
    }

    return 0;
}

int PipelineCache::create_shader_module(int shader_type_index, const Option& opt, uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z,
                                        VkShaderModule* _shader_module, ShaderInfo& si) const
{
#if NCNN_VULKAN_ONLINE_SPIRV
    std::vector<uint32_t> spirv;
    int retc = compile_spirv_module(shader_type_index, opt, spirv);
    if (retc != 0)
    {
        NCNN_LOGE("compile_spirv_module failed %d", retc);
        return -1;
    }

    const uint32_t* spv_data = spirv.data();
    size_t spv_data_size = spirv.size() * 4;

    int ret = resolve_shader_info(spv_data, spv_data_size, si);
    if (ret != 0)
    {
        NCNN_LOGE("resolve_shader_info failed %d", ret);
        return -1;
    }

//     if (vkdev->info.bug_local_size_spec_const)
//     {
    VkShaderModule shader_module = vkdev->compile_shader_module(spv_data, spv_data_size, local_size_x, local_size_y, local_size_z);
//     }
//     else
//     {
//         shader_module = vkdev->compile_shader_module(spv_data, spv_data_size);
//     }

#else // NCNN_VULKAN_ONLINE_SPIRV
    // ncnn_add_shader cmake macro
    // 0 = fp32
    // 1 = fp16p
    // 2 = fp16pa
    // 3 = fp16s
    // 4 = fp16sa
    // 5 = image
    // 6 = image_fp16p
    // 7 = image_fp16pa
    // 8 = image_fp16s
    // 9 = image_fp16sa

    if (!vkdev->info.bug_layout_binding_id_alias && opt.use_image_storage && vkdev->info.support_fp16_storage && opt.use_fp16_storage && vkdev->info.support_fp16_arithmetic && opt.use_fp16_arithmetic)
    {
        shader_type_index += 9;
    }
    else if (!vkdev->info.bug_layout_binding_id_alias && opt.use_image_storage && vkdev->info.support_fp16_packed && opt.use_fp16_packed && vkdev->info.support_fp16_arithmetic && opt.use_fp16_arithmetic)
    {
        shader_type_index += 7;
    }
    else if (!vkdev->info.bug_layout_binding_id_alias && opt.use_image_storage && vkdev->info.support_fp16_storage && opt.use_fp16_storage)
    {
        shader_type_index += 8;
    }
    else if (!vkdev->info.bug_layout_binding_id_alias && opt.use_image_storage && vkdev->info.support_fp16_packed && opt.use_fp16_packed)
    {
        shader_type_index += 6;
    }
    else if (!vkdev->info.bug_layout_binding_id_alias && opt.use_image_storage)
    {
        shader_type_index += 5;
    }
    else if (vkdev->info.support_fp16_storage && opt.use_fp16_storage && vkdev->info.support_fp16_arithmetic && opt.use_fp16_arithmetic)
    {
        shader_type_index += 4;
    }
    else if (vkdev->info.support_fp16_packed && opt.use_fp16_packed && vkdev->info.support_fp16_arithmetic && opt.use_fp16_arithmetic)
    {
        shader_type_index += 2;
    }
    else if (vkdev->info.support_fp16_storage && opt.use_fp16_storage)
    {
        shader_type_index += 3;
    }
    else if (vkdev->info.support_fp16_packed && opt.use_fp16_packed)
    {
        shader_type_index += 1;
    }

    si = get_shader_info(shader_type_index);

//     if (vkdev->info.bug_local_size_spec_const)
//     {
    VkShaderModule shader_module = vkdev->create_shader_module(shader_type_index, local_size_x, local_size_y, local_size_z);
//     }
//     else
//     {
//         shader_module = vkdev->get_shader_module(shader_type_index);
//     }

#endif // NCNN_VULKAN_ONLINE_SPIRV

    if (!shader_module)
    {
        NCNN_LOGE("create_shader_module failed");
        return -1;
    }

    *_shader_module = shader_module;

    return 0;
}

int PipelineCache::new_pipeline(VkShaderModule shader_module, const ShaderInfo& shader_info, const std::vector<vk_specialization_type>& specializations,
                                VkDescriptorSetLayout* _descriptorset_layout,
                                VkPipelineLayout* _pipeline_layout,
                                VkPipeline* _pipeline,
                                VkDescriptorUpdateTemplateKHR* _descriptor_update_template) const
{
    int ret = 0;

    VkDescriptorSetLayout descriptorset_layout = 0;
    VkPipelineLayout pipeline_layout = 0;
    VkPipeline pipeline = 0;
    VkDescriptorUpdateTemplateKHR descriptor_update_template = 0;

    // create new pipeline
    if ((int)specializations.size() != shader_info.specialization_count)
    {
        NCNN_LOGE("pipeline specialization count mismatch, expect %d but got %d", shader_info.specialization_count, (int)specializations.size());
        goto ERROR;
    }

    ret = vkdev->create_descriptorset_layout(shader_info.binding_count, shader_info.binding_types, &descriptorset_layout);
    if (ret != 0)
        goto ERROR;

    ret = vkdev->create_pipeline_layout(shader_info.push_constant_count, descriptorset_layout, &pipeline_layout);
    if (ret != 0)
        goto ERROR;

    ret = vkdev->create_pipeline(shader_module, pipeline_layout, specializations, &pipeline);
    if (ret != 0)
        goto ERROR;

    if (vkdev->info.support_VK_KHR_descriptor_update_template)
    {
        ret = vkdev->create_descriptor_update_template(shader_info.binding_count, shader_info.binding_types, descriptorset_layout, pipeline_layout, &descriptor_update_template);
        if (ret != 0)
            goto ERROR;
    }

    *_descriptorset_layout = descriptorset_layout;
    *_pipeline_layout = pipeline_layout;
    *_pipeline = pipeline;
    *_descriptor_update_template = descriptor_update_template;

    return 0;

ERROR:

    if (vkdev->info.support_VK_KHR_descriptor_update_template)
    {
        if (descriptor_update_template)
        {
            vkdev->vkDestroyDescriptorUpdateTemplateKHR(vkdev->vkdevice(), descriptor_update_template, 0);
        }
    }

    if (pipeline)
    {
        vkDestroyPipeline(vkdev->vkdevice(), pipeline, 0);
    }

    if (pipeline_layout)
    {
        vkDestroyPipelineLayout(vkdev->vkdevice(), pipeline_layout, 0);
    }

    if (descriptorset_layout)
    {
        vkDestroyDescriptorSetLayout(vkdev->vkdevice(), descriptorset_layout, 0);
    }

    return -1;
}

#endif // NCNN_VULKAN

} // namespace ncnn
