// ── ui_draw.h — custom 2D Vulkan draw layer for the Blender-style editor (replaces ImGui's renderer) ─────────
// A per-frame CPU vertex/index buffer (immediate-mode emit API: rect/border/line/text/image with clip stack) +
// a self-contained Vulkan pipeline (embedded SPIR-V, the font atlas as an R8 texture) recorded into the
// renderer's render pass via r->overlayDraw. Reuses VkRenderer's public helpers (createTextureImageRaw,
// createShaderModule, createBuffer, sharedSampler). Pixel coords, origin = top-left (Vulkan y-down clip).
// NOTE: included by editor.h AFTER vk_renderer.h, so Vulkan/volk types + VkRenderer are already declared.
#pragma once
#include "ui/ui_font.h"
#include "ui/ui_shaders_spv.h"
#include <vector>
#include <cstring>
#include <algorithm>

namespace ui {

inline uint32_t rgba(int r, int g, int b, int a = 255) {
    auto c=[](int v){ return (uint32_t)(v<0?0:v>255?255:v); };
    return c(r) | (c(g)<<8) | (c(b)<<16) | (c(a)<<24);
}
inline uint32_t withA(uint32_t col, int a) { return (col & 0x00FFFFFFu) | ((uint32_t)(a<0?0:a>255?255:a)<<24); }
inline uint32_t lerpCol(uint32_t a, uint32_t b, float t) {
    auto ch=[&](int s){ int x=(a>>s)&255, y=(b>>s)&255; return (uint32_t)(x+(y-x)*t)<<s; };
    return ch(0)|ch(8)|ch(16)|ch(24);
}

struct Vtx { float x, y, u, v; uint32_t col; };

// ── DrawList: builds geometry in pixel space with a clip (scissor) stack ──
struct DrawList {
    struct Cmd { uint32_t idxOff, idxCount; VkRect2D clip; };
    std::vector<Vtx> vtx; std::vector<uint32_t> idx; std::vector<Cmd> cmds;
    std::vector<VkRect2D> clipStk;
    Font* font = nullptr; float wu = 0.f, wv = 0.f;   // white-texel UV (solid fills)
    int W = 0, H = 0;

    void begin(int w, int h, Font* f, float whiteU, float whiteV) {
        vtx.clear(); idx.clear(); cmds.clear(); clipStk.clear();
        W = w; H = h; font = f; wu = whiteU; wv = whiteV;
        clipStk.push_back({{0,0},{(uint32_t)std::max(0,w),(uint32_t)std::max(0,h)}});
        cmds.push_back({0,0,clipStk.back()});
    }
    VkRect2D cur() const { return clipStk.back(); }
    void freshCmd() {                                  // start a new cmd whenever the clip changes
        VkRect2D c = cur();
        if (!cmds.empty() && cmds.back().idxCount == 0) { cmds.back().clip = c; return; }
        cmds.push_back({(uint32_t)idx.size(), 0, c});
    }
    void pushClip(float x, float y, float w, float h) {
        int x0=(int)x, y0=(int)y, x1=(int)(x+w), y1=(int)(y+h);
        VkRect2D p = cur();
        int px0=p.offset.x, py0=p.offset.y, px1=p.offset.x+(int)p.extent.width, py1=p.offset.y+(int)p.extent.height;
        x0=std::max(x0,px0); y0=std::max(y0,py0); x1=std::min(x1,px1); y1=std::min(y1,py1);
        if (x1<x0) x1=x0; if (y1<y0) y1=y0;
        clipStk.push_back({{x0,y0},{(uint32_t)(x1-x0),(uint32_t)(y1-y0)}});
        freshCmd();
    }
    void popClip() { if (clipStk.size()>1) clipStk.pop_back(); freshCmd(); }
    bool clipEmpty() const { VkRect2D c=cur(); return c.extent.width==0||c.extent.height==0; }

    void prim(const Vtx* v, int nv, const uint16_t* id, int ni) {
        if (clipEmpty()) return;
        if (cmds.empty()) freshCmd();
        uint32_t base = (uint32_t)vtx.size();
        vtx.insert(vtx.end(), v, v+nv);
        for (int i=0;i<ni;i++) idx.push_back(base + id[i]);
        cmds.back().idxCount += ni;
    }
    void rect(float x, float y, float w, float h, uint32_t col) {
        if (w<=0||h<=0) return;
        Vtx v[4]={{x,y,wu,wv,col},{x+w,y,wu,wv,col},{x+w,y+h,wu,wv,col},{x,y+h,wu,wv,col}};
        static const uint16_t id[6]={0,1,2,0,2,3}; prim(v,4,id,6);
    }
    void rectGradV(float x, float y, float w, float h, uint32_t top, uint32_t bot) {
        if (w<=0||h<=0) return;
        Vtx v[4]={{x,y,wu,wv,top},{x+w,y,wu,wv,top},{x+w,y+h,wu,wv,bot},{x,y+h,wu,wv,bot}};
        static const uint16_t id[6]={0,1,2,0,2,3}; prim(v,4,id,6);
    }
    void border(float x, float y, float w, float h, uint32_t col, float t=1.f) {
        rect(x,y,w,t,col); rect(x,y+h-t,w,t,col); rect(x,y+t,t,h-2*t,col); rect(x+w-t,y+t,t,h-2*t,col);
    }
    void line(float x0,float y0,float x1,float y1,uint32_t col,float thick=1.f){
        float dx=x1-x0,dy=y1-y0,len=std::sqrt(dx*dx+dy*dy); if(len<1e-4f)return;
        float nx=-dy/len*thick*0.5f, ny=dx/len*thick*0.5f;
        Vtx v[4]={{x0+nx,y0+ny,wu,wv,col},{x1+nx,y1+ny,wu,wv,col},{x1-nx,y1-ny,wu,wv,col},{x0-nx,y0-ny,wu,wv,col}};
        static const uint16_t id[6]={0,1,2,0,2,3}; prim(v,4,id,6);
    }
    void triangle(float ax,float ay,float bx,float by,float cx,float cy,uint32_t col){
        Vtx v[3]={{ax,ay,wu,wv,col},{bx,by,wu,wv,col},{cx,cy,wu,wv,col}};
        static const uint16_t id[3]={0,1,2}; prim(v,3,id,3);
    }
    // Draw text; returns the end-x. Baseline placed at y+ascent so y is the top of the line box.
    float text(float x, float y, const char* s, uint32_t col) {
        if (!font || !s) return x;
        float px = x, py = y + font->ascent;
        const char* p = s;
        while (*p) {
            unsigned c = i18n::utf8Next(p);                 // UTF-8 decode (CJK translations) — atlas has ASCII + baked CJK glyphs
            if (c < 32) { if (c=='\t') px += font->advance(' ')*4; continue; }
            stbtt_aligned_quad q; font->quad(c, &px, &py, &q);
            Vtx v[4]={{q.x0,q.y0,q.s0,q.t0,col},{q.x1,q.y0,q.s1,q.t0,col},{q.x1,q.y1,q.s1,q.t1,col},{q.x0,q.y1,q.s0,q.t1,col}};
            static const uint16_t id[6]={0,1,2,0,2,3}; prim(v,4,id,6);
        }
        return px;
    }
    float textW(const char* s) const { return font?font->textWidth(s):0.f; }
};

// ── UIDraw: owns the Vulkan pipeline, the font atlas, and per-frame device buffers ──
struct UIDraw {
    VkRenderer* r = nullptr; Font* font = nullptr;
    VkDescriptorSetLayout dsl=VK_NULL_HANDLE; VkDescriptorPool dpool=VK_NULL_HANDLE; VkDescriptorSet dset=VK_NULL_HANDLE;
    VkPipelineLayout playout=VK_NULL_HANDLE; VkPipeline pipe=VK_NULL_HANDLE;
    VkImage atlas=VK_NULL_HANDLE; VkDeviceMemory atlasMem=VK_NULL_HANDLE; VkImageView atlasView=VK_NULL_HANDLE;
    VkBuffer vbo=VK_NULL_HANDLE, ibo=VK_NULL_HANDLE; VkDeviceMemory vboMem=VK_NULL_HANDLE, iboMem=VK_NULL_HANDLE;
    size_t vboCap=0, iboCap=0;
    float whiteU=0,whiteV=0;

    void init(VkRenderer* rr, Font* f) {
        r = rr; font = f;
        int aw=f->atlasW, ah=f->atlasH;
        for (int yy=ah-6; yy<ah-2 && yy>=0; ++yy) for (int xx=aw-6; xx<aw-2 && xx>=0; ++xx) f->pixels[(size_t)yy*aw+xx]=255;
        whiteU=(aw-4.0f)/aw; whiteV=(ah-4.0f)/ah;
        r->createTextureImageRaw(f->pixels.data(), (uint32_t)f->pixels.size(), aw, ah, VK_FORMAT_R8_UNORM, atlas, atlasMem);
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image=atlas; iv.viewType=VK_IMAGE_VIEW_TYPE_2D; iv.format=VK_FORMAT_R8_UNORM;
        iv.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        // identity swizzle: the frag shader reads .r as glyph coverage (an ONE swizzle made every glyph a solid block)
        iv.components={VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
        vkCreateImageView(r->device,&iv,nullptr,&atlasView);

        VkDescriptorSetLayoutBinding b{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=1; dl.pBindings=&b;
        vkCreateDescriptorSetLayout(r->device,&dl,nullptr,&dsl);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=1; dp.pPoolSizes=&ps;
        vkCreateDescriptorPool(r->device,&dp,nullptr,&dpool);
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=dpool; da.descriptorSetCount=1; da.pSetLayouts=&dsl;
        vkAllocateDescriptorSets(r->device,&da,&dset);
        VkDescriptorImageInfo di{r->sharedSampler, atlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=dset; w.dstBinding=0; w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&di; vkUpdateDescriptorSets(r->device,1,&w,0,nullptr);

        VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(float)*2};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&dsl; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&pc;
        vkCreatePipelineLayout(r->device,&pl,nullptr,&playout);
        createPipeline();
    }

    void createPipeline() {
        std::vector<uint32_t> vs(UI_VERT_SPV, UI_VERT_SPV + UI_VERT_SPV_size/4), fs(UI_FRAG_SPV, UI_FRAG_SPV + UI_FRAG_SPV_size/4);
        VkShaderModule vm=r->createShaderModule(vs), fm=r->createShaderModule(fs);
        VkPipelineShaderStageCreateInfo st[2]={};
        st[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; st[0].stage=VK_SHADER_STAGE_VERTEX_BIT; st[0].module=vm; st[0].pName="main";
        st[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module=fm; st[1].pName="main";
        VkVertexInputBindingDescription bind{0,sizeof(Vtx),VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription at[3]={
            {0,0,VK_FORMAT_R32G32_SFLOAT,(uint32_t)offsetof(Vtx,x)},
            {1,0,VK_FORMAT_R32G32_SFLOAT,(uint32_t)offsetof(Vtx,u)},
            {2,0,VK_FORMAT_R8G8B8A8_UNORM,(uint32_t)offsetof(Vtx,col)} };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind; vi.vertexAttributeDescriptionCount=3; vi.pVertexAttributeDescriptions=at;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=1; vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.lineWidth=1.f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable=VK_FALSE; ds.depthWriteEnable=VK_FALSE;
        VkPipelineColorBlendAttachmentState cba{}; cba.blendEnable=VK_TRUE;
        cba.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp=VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.alphaBlendOp=VK_BLEND_OP_ADD;
        cba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount=1; cb.pAttachments=&cba;
        VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds2{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds2.dynamicStateCount=2; ds2.pDynamicStates=dyn;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount=2; pci.pStages=st; pci.pVertexInputState=&vi; pci.pInputAssemblyState=&ia; pci.pViewportState=&vp;
        pci.pRasterizationState=&rs; pci.pMultisampleState=&ms; pci.pDepthStencilState=&ds; pci.pColorBlendState=&cb; pci.pDynamicState=&ds2;
        pci.layout=playout; pci.renderPass=r->renderPass; pci.subpass=0;
        vkCreateGraphicsPipelines(r->device,VK_NULL_HANDLE,1,&pci,nullptr,&pipe);
        vkDestroyShaderModule(r->device,vm,nullptr); vkDestroyShaderModule(r->device,fm,nullptr);
    }

    // Re-upload the atlas after the caller re-baked font->pixels at a new size (responsive UI scaling). Safe to call
    // from overlayBegin/buildFrame: that runs after swapchain-recreate and before this frame's draw is recorded, so
    // vkDeviceWaitIdle here only waits out prior frames (none reference the about-to-be-destroyed atlas).
    void reloadFont() {
        if (!r || !font) return;
        int aw=font->atlasW, ah=font->atlasH;
        for (int yy=ah-6; yy<ah-2 && yy>=0; ++yy) for (int xx=aw-6; xx<aw-2 && xx>=0; ++xx) font->pixels[(size_t)yy*aw+xx]=255;
        whiteU=(aw-4.0f)/aw; whiteV=(ah-4.0f)/ah;
        vkDeviceWaitIdle(r->device);
        if (atlasView) vkDestroyImageView(r->device,atlasView,nullptr);
        if (atlas)     vkDestroyImage(r->device,atlas,nullptr);
        if (atlasMem)  vkFreeMemory(r->device,atlasMem,nullptr);
        r->createTextureImageRaw(font->pixels.data(), (uint32_t)font->pixels.size(), aw, ah, VK_FORMAT_R8_UNORM, atlas, atlasMem);
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image=atlas; iv.viewType=VK_IMAGE_VIEW_TYPE_2D; iv.format=VK_FORMAT_R8_UNORM;
        iv.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        iv.components={VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
        vkCreateImageView(r->device,&iv,nullptr,&atlasView);
        VkDescriptorImageInfo di{r->sharedSampler, atlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=dset; w.dstBinding=0; w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&di; vkUpdateDescriptorSets(r->device,1,&w,0,nullptr);
    }

    void ensureBuf(VkBuffer& buf, VkDeviceMemory& mem, size_t& cap, size_t need, VkBufferUsageFlags usage) {
        if (need <= cap) return;
        if (buf) { vkDestroyBuffer(r->device,buf,nullptr); vkFreeMemory(r->device,mem,nullptr); }
        cap = need + need/2 + 4096;
        r->createBuffer(cap, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf, mem);
    }

    void record(VkCommandBuffer cmd, DrawList& dl) {
        if (dl.idx.empty()) return;
        size_t vb = dl.vtx.size()*sizeof(Vtx), ib = dl.idx.size()*sizeof(uint32_t);
        ensureBuf(vbo,vboMem,vboCap,vb,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        ensureBuf(ibo,iboMem,iboCap,ib,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        void* p; vkMapMemory(r->device,vboMem,0,vb,0,&p); memcpy(p,dl.vtx.data(),vb); vkUnmapMemory(r->device,vboMem);
        vkMapMemory(r->device,iboMem,0,ib,0,&p); memcpy(p,dl.idx.data(),ib); vkUnmapMemory(r->device,iboMem);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,playout,0,1,&dset,0,nullptr);
        float pc[2]={2.0f/(float)dl.W, 2.0f/(float)dl.H}; vkCmdPushConstants(cmd,playout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(pc),pc);
        VkViewport vp{0,0,(float)dl.W,(float)dl.H,0,1}; vkCmdSetViewport(cmd,0,1,&vp);
        VkDeviceSize off=0; vkCmdBindVertexBuffers(cmd,0,1,&vbo,&off); vkCmdBindIndexBuffer(cmd,ibo,0,VK_INDEX_TYPE_UINT32);
        for (auto& c : dl.cmds) {
            if (c.idxCount==0 || c.clip.extent.width==0 || c.clip.extent.height==0) continue;
            vkCmdSetScissor(cmd,0,1,&c.clip);
            vkCmdDrawIndexed(cmd, c.idxCount, 1, c.idxOff, 0, 0);
        }
    }

    void destroy() {
        if (!r) return;
        if (pipe) vkDestroyPipeline(r->device,pipe,nullptr);
        if (playout) vkDestroyPipelineLayout(r->device,playout,nullptr);
        if (dpool) vkDestroyDescriptorPool(r->device,dpool,nullptr);
        if (dsl) vkDestroyDescriptorSetLayout(r->device,dsl,nullptr);
        if (atlasView) vkDestroyImageView(r->device,atlasView,nullptr);
        if (atlas) vkDestroyImage(r->device,atlas,nullptr);
        if (atlasMem) vkFreeMemory(r->device,atlasMem,nullptr);
        if (vbo) { vkDestroyBuffer(r->device,vbo,nullptr); vkFreeMemory(r->device,vboMem,nullptr); }
        if (ibo) { vkDestroyBuffer(r->device,ibo,nullptr); vkFreeMemory(r->device,iboMem,nullptr); }
    }
};

} // namespace ui
