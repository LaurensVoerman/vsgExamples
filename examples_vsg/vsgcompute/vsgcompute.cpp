#include <vsg/all.h>

#include <iostream>
#include <chrono>

int main(int argc, char** argv)
{
    vsg::CommandLine arguments(&argc, argv);
    std::uint32_t width = 1024, height = 1024; // data/shader/shader.comp is currently hardwired to 1024x1024.
    //auto [width, height] = arguments.value(std::pair<uint32_t, uint32_t>(1024, 1024), {"--size", "-s"}); // TODO pass uniform to shader with sizes
    auto debugLayer = arguments.read({"--debug","-d"});
    auto apiDumpLayer = arguments.read({"--api","-a"});
    auto workgroupSize = arguments.value<uint32_t>(32, "-w");
    auto outputFilename = arguments.value<std::string>("", "-o");
    auto outputAsFloat = arguments.read("-f");
    if (arguments.errors()) return arguments.writeErrorMessages(std::cerr);

    vsg::Names instanceExtensions;
    vsg::Names requestedLayers;
    vsg::Names deviceExtensions;
    if (debugLayer)
    {
        instanceExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        requestedLayers.push_back("VK_LAYER_LUNARG_standard_validation");
        if (apiDumpLayer) requestedLayers.push_back("VK_LAYER_LUNARG_api_dump");
    }

    vsg::Paths searchPaths = vsg::getEnvPaths("VSG_FILE_PATH");
    vsg::ref_ptr<vsg::Shader> computeShader = vsg::Shader::read(VK_SHADER_STAGE_COMPUTE_BIT, "main", vsg::findFile("shaders/comp.spv", searchPaths));
    if (!computeShader)
    {
        std::cout<<"Error : No shader loaded."<<std::endl;
        return 1;
    }


    vsg::Names validatedNames = vsg::validateInstancelayerNames(requestedLayers);

    vsg::ref_ptr<vsg::Instance> instance = vsg::Instance::create(instanceExtensions, validatedNames);
    vsg::ref_ptr<vsg::PhysicalDevice> physicalDevice = vsg::PhysicalDevice::create(instance, VK_QUEUE_COMPUTE_BIT);
    vsg::ref_ptr<vsg::Device> device = vsg::Device::create(physicalDevice, validatedNames, deviceExtensions);
    if (!device)
    {
        std::cout<<"Unable to create required Vulkan Device."<<std::endl;
        return 1;
    }

    // get the queue for the compute commands
    VkQueue computeQueue = device->getQueue(physicalDevice->getComputeFamily());


    // allocate output storage buffer
    VkDeviceSize bufferSize = sizeof(vsg::vec4) * width * height;
    vsg::ref_ptr<vsg::Buffer> buffer =  vsg::Buffer::create(device, bufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE);
    vsg::ref_ptr<vsg::DeviceMemory>  bufferMemory = vsg::DeviceMemory::create(device, buffer,  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    buffer->bind(bufferMemory, 0);


    // set up DescriptorPool, DescriptorSetLayout, DecriptorSet and BindDescriptorSets
    vsg::ref_ptr<vsg::DescriptorPool> descriptorPool = vsg::DescriptorPool::create(device, 1,
    {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}
    });

    vsg::ref_ptr<vsg::DescriptorSetLayout> descriptorSetLayout = vsg::DescriptorSetLayout::create(device,
    {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
    });

    vsg::ref_ptr<vsg::DescriptorSet> descriptorSet = vsg::DescriptorSet::create(device, descriptorPool, descriptorSetLayout,
    {
        vsg::DescriptorBuffer::create(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vsg::BufferDataList{vsg::BufferData(buffer, 0, bufferSize)})
    });

    vsg::ref_ptr<vsg::PipelineLayout> pipelineLayout = vsg::PipelineLayout::create(device, {descriptorSetLayout}, {});

    vsg::ref_ptr<vsg::BindDescriptorSets> bindDescriptorSets = vsg::BindDescriptorSets::create(VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, vsg::DescriptorSets{descriptorSet});


    // set up the compute pipeline
    vsg::ref_ptr<vsg::ShaderModule> computeShaderModule = vsg::ShaderModule::create(device, computeShader);
    vsg::ref_ptr<vsg::ComputePipeline> pipeline = vsg::ComputePipeline::create(device, pipelineLayout, computeShaderModule);
    vsg::ref_ptr<vsg::BindPipeline> bindPipeline = vsg::BindPipeline::create(pipeline);


    // setup command pool
    vsg::ref_ptr<vsg::CommandPool> commandPool = vsg::CommandPool::create(device, physicalDevice->getComputeFamily());


    // setup fence
    vsg::ref_ptr<vsg::Fence> fence = vsg::Fence::create(device);

    auto startTime =std::chrono::steady_clock::now();

    // dispatch commands
    vsg::dispatchCommandsToQueue(device, commandPool, fence, 100000000000, computeQueue, [&](vsg::CommandBuffer& commandBuffer)
    {
        bindPipeline->dispatch(commandBuffer);
        bindDescriptorSets->dispatch(commandBuffer);
        vkCmdDispatch(commandBuffer, uint32_t(ceil(float(width)/float(workgroupSize))), uint32_t(ceil(float(height)/float(workgroupSize))), 1);
    });

    auto time = std::chrono::duration<float, std::chrono::milliseconds::period>(std::chrono::steady_clock::now()-startTime).count();
    std::cout<<"Time to run commands "<<time<<"ms"<<std::endl;

    if (!outputFilename.empty())
    {
        // Map the buffer memory and assign as a vec4Array2D that will automatically unmap itself on destruction.
        vsg::ref_ptr<vsg::vec4Array2D> image(new vsg::MappedData<vsg::vec4Array2D>(bufferMemory, 0, 0, width, height)); // deviceMemory, offset, flags and dimensions
        image->setFormat(VK_FORMAT_R32G32B32A32_SFLOAT);

        if (outputAsFloat)
        {
            vsg::vsgReaderWriter io;
            io.writeFile(image, outputFilename);
        }
        else
        {
            // create a unsigned byte version of the image and then copy the texels across converting colours from float to unsigned byte.
            vsg::ref_ptr<vsg::ubvec4Array2D> dest(new vsg::ubvec4Array2D(width, height));
            dest->setFormat(VK_FORMAT_R8G8B8A8_UNORM);

            auto c_itr = dest->begin();
            for(auto& colour : *image)
            {
                (c_itr++)->set(colour.r*255.0, colour.g*255.0, colour.b*255.0, colour.a*255.0);
            }

            vsg::vsgReaderWriter io;
            io.writeFile(dest, outputFilename);
        }
    }

    // clean up done automatically thanks to ref_ptr<>
    return 0;
}
