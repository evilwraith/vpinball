// license:GPLv3+

#include "core/stdafx.h"
#include "VertexBuffer.h"
#include "RenderDevice.h"
#include "Shader.h"


class SharedVertexBuffer final : public SharedBuffer<VertexFormat, VertexBuffer>
{
public:
   SharedVertexBuffer(RenderDevice* const rd, VertexFormat fmt, bool stat);
   ~SharedVertexBuffer();
   void Upload() override;

   #if defined(ENABLE_BGFX)
   bgfx::VertexBufferHandle m_vb = BGFX_INVALID_HANDLE;
   bgfx::DynamicVertexBufferHandle m_dvb = BGFX_INVALID_HANDLE;
   bool IsCreated() const override { return m_isStatic ? bgfx::isValid(m_vb) : bgfx::isValid(m_dvb); }
   const bgfx::VertexLayout* const m_vertexDeclaration;
   #ifdef __RK3588__
   // mali-optimized.md §8, ported: the Mali blob stalls ~0.4 ms on every write into GPU storage an
   // in-flight draw references, and (without the eglSwapBuffers drain that used to mask it) tears
   // instead. Dynamic buffer content therefore lives in a CPU shadow; each frame that draws the
   // buffer takes one transient snapshot of the whole shared block, so no GL storage is ever
   // written while the GPU might read it. The persistent m_dvb keeps only the creation-time
   // content, as a fallback for transient-ring exhaustion.
   uint8_t* m_shadow = nullptr;
   bgfx::TransientVertexBuffer m_transient {};
   uint32_t m_transientFrame = 0xFFFFFFFFu;
   bool UseCpuShadow() const override { return m_shadow != nullptr; }
   const bgfx::TransientVertexBuffer* GetFrameTransient();
   #endif

   #elif defined(ENABLE_OPENGL)
   GLuint m_vb = 0;
   void Bind() const { glBindBuffer(GL_ARRAY_BUFFER, m_vb); }
   bool IsCreated() const override { return m_vb; }

   #elif defined(ENABLE_DX9)
   IDirect3DVertexBuffer9* m_vb = nullptr;
   IDirect3DVertexDeclaration9* const m_vertexDeclaration;
   bool IsCreated() const override { return m_vb; }
   #endif
};

SharedVertexBuffer::SharedVertexBuffer(RenderDevice* const rd, VertexFormat fmt, bool stat)
   : SharedBuffer(fmt, fmt ==  VertexFormat::VF_POS_NORMAL_TEX ? sizeof(Vertex3D_NoTex2) : sizeof(Vertex3D_TexelOnly), stat)
#if defined(ENABLE_DX9) || defined(ENABLE_BGFX)
   , m_vertexDeclaration(
      fmt == VertexFormat::VF_POS_NORMAL_TEX ? rd->m_pVertexNormalTexelDeclaration :
      fmt == VertexFormat::VF_POS_TEX        ? rd->m_pVertexTexelDeclaration : nullptr)
#endif
{
}

SharedVertexBuffer::~SharedVertexBuffer()
{
   #if defined(ENABLE_BGFX) && defined(__RK3588__)
   delete[] m_shadow;
   #endif
   if (IsCreated())
   {
      #if defined(ENABLE_BGFX)
      if (bgfx::isValid(m_vb))
         bgfx::destroy(m_vb);
      if (bgfx::isValid(m_dvb))
         bgfx::destroy(m_dvb);
      #elif defined(ENABLE_OPENGL)
      glDeleteBuffers(1, &m_vb);
      #elif defined(ENABLE_DX9)
      SAFE_RELEASE(m_vb);
      #endif
   }
   #if defined(ENABLE_BGFX)
   for (const PendingUpload& upload : m_pendingUploads)
      if (upload.mem)
         delete upload.mem;
      else
         delete[] upload.data;
   #else
   for (const PendingUpload& upload : m_pendingUploads)
      delete[] upload.data;
   #endif
}

void SharedVertexBuffer::Upload()
{
   if (!IsCreated())
   {
      const unsigned int size = m_count * m_bytePerElement;

      // Create data block
      #if defined(ENABLE_BGFX)
      const bgfx::Memory* mem = bgfx::alloc(size);
      uint8_t* data = mem->data;

      #elif defined(ENABLE_OPENGL)
      uint8_t* const data = new uint8_t[size];

      #elif defined(ENABLE_DX9)
      // We always specify WRITEONLY since MSDN states,
      // "Buffers created with D3DPOOL_DEFAULT that do not specify D3DUSAGE_WRITEONLY may suffer a severe performance penalty."
      // This means we cannot read from vertex buffers, but I don't think we need to.
      CHECKD3D(m_buffers[0]->m_rd->GetCoreDevice()->CreateVertexBuffer(size, D3DUSAGE_WRITEONLY | (m_isStatic ? 0 : D3DUSAGE_DYNAMIC), 0 /* sharedBuffer->format */, D3DPOOL_DEFAULT, &m_vb, nullptr));
      uint8_t* data;
      CHECKD3D(m_vb->Lock(0, size, (void**)&data, 0));

      #endif

      // Fill data block
      for (const PendingUpload& upload : m_pendingUploads)
      {
         //assert(upload.offset >= 0);
         assert(upload.offset + upload.size <= size);
         memcpy(data + upload.offset, upload.data, upload.size);
         delete[] upload.data;
      }
      m_pendingUploads.clear();

      // Upload data block
      #if defined(ENABLE_BGFX)
      #ifdef __RK3588__
      // Snapshot the initial content: from here on, updates land in the shadow and draws take
      // per-frame transient copies (see the member comment).
      if (!m_isStatic && RenderDevice::s_dynBufferShadow)
      {
         m_shadow = new uint8_t[size];
         memcpy(m_shadow, data, size);
      }
      #endif
      if (m_isStatic)
         m_vb = bgfx::createVertexBuffer(mem, *m_vertexDeclaration, BGFX_BUFFER_NONE);
      else
         m_dvb = bgfx::createDynamicVertexBuffer(mem, *m_vertexDeclaration, BGFX_BUFFER_NONE);

      #elif defined(ENABLE_OPENGL)
         #ifndef __OPENGLES__
         if (GLAD_GL_VERSION_4_5)
         {
            glCreateBuffers(1, &m_vb);
            glNamedBufferStorage(m_vb, size, data, m_isStatic ? 0 : GL_DYNAMIC_STORAGE_BIT);
         }
         else if (GLAD_GL_VERSION_4_4)
         {
            glGenBuffers(1, &m_vb);
            Bind();
            glBufferStorage(GL_ARRAY_BUFFER, size, data, m_isStatic ? 0 : GL_DYNAMIC_STORAGE_BIT);
         }
         else
         #endif
      {
         glGenBuffers(1, &m_vb);
         Bind();
         glBufferData(GL_ARRAY_BUFFER, size, data, m_isStatic ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW);
      }
      delete [] data;

      #elif defined(ENABLE_DX9)
      CHECKD3D(m_vb->Unlock());

      #endif
   }
   else
   {
      for (const PendingUpload& upload : m_pendingUploads)
      {
         assert(!m_isStatic);
         #if defined(ENABLE_BGFX)
         #ifdef __RK3588__
         if (m_shadow != nullptr)
         {
            // Shadow mode: no GPU write here at all; the next GetFrameTransient() snapshots it.
            memcpy(m_shadow + upload.offset, upload.data, upload.size);
            delete[] upload.data;
            ++RenderDevice::s_dynVbUpdates;
            RenderDevice::s_dynVbBytes += upload.size;
            continue;
         }
         #endif
         bgfx::update(m_dvb, upload.offset / m_bytePerElement, upload.mem);
         #ifdef __RK3588__
         ++RenderDevice::s_dynVbUpdates;
         RenderDevice::s_dynVbBytes += upload.size;
         #endif

         #elif defined(ENABLE_OPENGL)
         #ifndef __OPENGLES__
         if (GLAD_GL_VERSION_4_5)
            glNamedBufferSubData(m_vb, upload.offset, upload.size, upload.data);
         else
         #endif
         {
            Bind();
            glBufferSubData(GL_ARRAY_BUFFER, upload.offset, upload.size, upload.data);
         }
         delete[] upload.data;

         #elif defined(ENABLE_DX9)
         // It would be better to perform a single lock but in fact, I don't think there are situations where more than one update is pending
         uint8_t* data;
         CHECKD3D(m_vb->Lock(upload.offset, upload.size, (void**)&data, 0));
         memcpy(data, upload.data, upload.size);
         CHECKD3D(m_vb->Unlock());
         delete[] upload.data;

         #endif
      }
      m_pendingUploads.clear();
   }
}

#if defined(ENABLE_BGFX) && defined(__RK3588__)
const bgfx::TransientVertexBuffer* SharedVertexBuffer::GetFrameTransient()
{
   if (m_shadow == nullptr)
      return nullptr;
   if (m_transientFrame != RenderDevice::s_frameIndex)
   {
      if (bgfx::getAvailTransientVertexBuffer(m_count, *m_vertexDeclaration) < m_count)
      {
         // Ring exhausted: fall back to the persistent buffer (stale content beats a crash).
         static bool s_warned = false;
         if (!s_warned)
         {
            s_warned = true;
            PLOGE.printf("[4kpDebug][dyn_shadow] transient vertex ring exhausted (%u vertices wanted); raise maxTransientVbSize", m_count);
         }
         return nullptr;
      }
      bgfx::allocTransientVertexBuffer(&m_transient, m_count, *m_vertexDeclaration);
      memcpy(m_transient.data, m_shadow, size_t(m_count) * m_bytePerElement);
      m_transientFrame = RenderDevice::s_frameIndex;
   }
   return &m_transient;
}

const bgfx::TransientVertexBuffer* VertexBuffer::GetFrameTransient() const { return m_sharedBuffer->GetFrameTransient(); }
#endif


VertexBuffer::VertexBuffer(RenderDevice* rd, const unsigned int vertexCount, const float* verts, const bool isDynamic, const VertexFormat fmt)
   : m_rd(rd)
   , m_isStatic(!isDynamic)
   , m_vertexFormat(fmt)
   , m_count(vertexCount)
   , m_sizePerVertex(fmt ==  VertexFormat::VF_POS_NORMAL_TEX ? sizeof(Vertex3D_NoTex2) : sizeof(Vertex3D_TexelOnly))
   , m_size(vertexCount * (fmt ==  VertexFormat::VF_POS_NORMAL_TEX ? sizeof(Vertex3D_NoTex2) : sizeof(Vertex3D_TexelOnly)))
{
   assert(m_count > 0);
   // Disabled since OpenGL ES does not support glDrawElementsBaseVertex, but now that we remap the indices when creating the index buffer it should be good
   for (std::shared_ptr<SharedVertexBuffer> block : m_rd->m_pendingSharedVertexBuffers)
   {
      if (block->m_format == fmt && block->m_isStatic == m_isStatic && block->GetCount() + vertexCount <= 65535)
      {
         m_sharedBuffer = block;
         break;
      }
   }
   // Also create a new buffer when using dynamic buffers as all backends do not support vertex offsets
   if (!m_isStatic || m_sharedBuffer == nullptr)
   {
      m_sharedBuffer = std::make_shared<SharedVertexBuffer>(rd, fmt, m_isStatic);
      m_rd->m_pendingSharedVertexBuffers.push_back(m_sharedBuffer);
   }
   m_vertexOffset = m_sharedBuffer->Add(this);
   m_offset = m_vertexOffset * m_sizePerVertex;
   if (verts != nullptr)
   {
      void* data;
      Lock(data);
      memcpy(data, verts, m_size);
      Unlock();
   }
}

VertexBuffer::~VertexBuffer()
{
   if (m_sharedBuffer->Remove(this))
      RemoveFromVectorSingle(m_rd->m_pendingSharedVertexBuffers, m_sharedBuffer);
}

bool VertexBuffer::IsSharedBuffer() const { return m_sharedBuffer->IsShared(); }

#if defined(ENABLE_BGFX)
bgfx::VertexBufferHandle VertexBuffer::GetStaticBuffer() const { return m_sharedBuffer->m_vb; }
bgfx::DynamicVertexBufferHandle VertexBuffer::GetDynamicBuffer() const { return m_sharedBuffer->m_dvb; }

#elif defined(ENABLE_OPENGL)
GLuint VertexBuffer::GetBuffer() const { return m_sharedBuffer->m_vb; }
void VertexBuffer::Bind() const { m_sharedBuffer->Bind(); }

#elif defined(ENABLE_DX9)
IDirect3DVertexBuffer9* VertexBuffer::GetBuffer() const { return m_sharedBuffer->m_vb; }
void VertexBuffer::Bind() const
{
   if (m_rd->m_curVertexBuffer != m_sharedBuffer->m_vb)
   {
      CHECKD3D(m_rd->GetCoreDevice()->SetStreamSource(0, m_sharedBuffer->m_vb, 0, m_sizePerVertex));
      m_rd->m_curVertexBuffer = m_sharedBuffer->m_vb;
   }
   if (m_rd->m_currentVertexDeclaration != m_sharedBuffer->m_vertexDeclaration)
   {
      CHECKD3D(m_rd->GetCoreDevice()->SetVertexDeclaration(m_sharedBuffer->m_vertexDeclaration));
      m_rd->m_currentVertexDeclaration = m_sharedBuffer->m_vertexDeclaration;
      m_rd->m_curStateChanges++;
   }
}

#endif

void VertexBuffer::LockUntyped(void*& data, const unsigned int offset, const unsigned int size)
{
   m_rd->m_curLockCalls++;
   m_sharedBuffer->Lock(this, m_offset + offset, size == 0 ? m_size : size, data);
}

void VertexBuffer::Unlock()
{
   m_sharedBuffer->Unlock();
}

void VertexBuffer::Upload()
{
   if (!m_sharedBuffer->IsCreated())
      RemoveFromVectorSingle(m_rd->m_pendingSharedVertexBuffers, m_sharedBuffer);
   m_sharedBuffer->Upload();
}
