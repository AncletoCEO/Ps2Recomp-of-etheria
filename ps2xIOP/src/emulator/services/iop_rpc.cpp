#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // 2026-10-09 (plataforma): sids que en la consola real provee el IOP BIOS
        // (IOPRP300): sifcmd (0x80000592, 0x8000059c) y cdvdfsv (0x80000006). El
        // recomp no carga IOPRP300, así que el IOP emulado no tiene sus servidores
        // y el bind del EE queda en client->server=0 (el SDK reintenta). Con este
        // flag se exponen como servidores HLE para que el bind prospere.
        bool isIopBiosSid(uint32_t sid) noexcept
        {
            static const bool enabled = []() {
                const char *e = std::getenv("SWORD_IOP_BIOS_RPC");
                return e != nullptr && *e != '0';
            }();
            if (!enabled)
                return false;
            return sid == 0x80000592u || sid == 0x8000059cu || sid == 0x80000006u;
        }
    }

    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host)
        , m_memory(memory)
        , m_kernel(kernel)
    {
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_nextDmaId = 1u;
        // 2026-10-09 (plataforma): en la consola real el IOP BIOS (IOPRP300)
        // inicializa sifman ANTES de cargar los IRX del juego. Sin esto, módulos
        // como PADMAN/DBCMAN ven sceSifCheckInit()==0 y avisan "yet sif hasn't
        // been init". SWORD_IOP_SIF_INIT=0 para A/B.
        static const bool preinit = []() {
            const char *e = std::getenv("SWORD_IOP_SIF_INIT");
            return e != nullptr && *e != '0';
        }();
        m_sifInitialized = preinit;
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma
        {
            constexpr uint32_t kDescriptorSize = 16u;
            constexpr uint32_t kMaxDescriptors = 32u;
            const uint32_t descriptorAddress = cpu.gpr[4];
            const uint32_t descriptorCount = cpu.gpr[5];
            if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
            {
                setV0(0u);
                return true;
            }

            struct PendingTransfer
            {
                uint32_t source = 0u;
                uint32_t destination = 0u;
                uint32_t size = 0u;
            };

            std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
            const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
            if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
            {
                setV0(0u);
                return true;
            }

            std::array<PendingTransfer, kMaxDescriptors> pending{};
            uint32_t pendingCount = 0u;
            uint32_t largestTransfer = 0u;
            for (uint32_t i = 0u; i < descriptorCount; ++i)
            {
                const uint32_t source = descriptorWords[i * 4u + 0u];
                const uint32_t destination = descriptorWords[i * 4u + 1u];
                const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
                if (signedSize <= 0)
                    continue;

                const uint32_t size = static_cast<uint32_t>(signedSize);
                if (!m_memory.ownsRamRange(source, size))
                {
                    setV0(0u);
                    return true;
                }
                pending[pendingCount++] = {source, destination, size};
                largestTransfer = std::max(largestTransfer, size);
            }

            // IOP-side sceSifSetDma sends IOP RAM to the EE. Validate all EE
            // destinations before committing any write so a bad chain cannot
            // partially update guest memory, but maybe we could skip this check if we trust the EE-side SIF driver to validate the chain ?!
            // TODO check later
            std::vector<uint8_t> scratch(largestTransfer);
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_host.readGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) || !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            {
                m_nextDmaId = 1u;
            }
            setV0(dmaId);
            return true;
        }
        case 8: // sceSifDmaStat
            setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        default:
            setV0(0u);
            return true;
        }
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // InitCmd
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 14: // InitRpc
        case 15:
        case 16:
            setV0(0);
            return true;
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize ||
                !m_memory.ownsRamRange(packetAddress, packetSize))
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                if (extraSource == 0u || extraDestination == 0u ||
                    !m_memory.ownsRamRange(extraSource, extraSize) ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + IopMemory::physicalAddress(extraSource), extraSize))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            if (!m_host.sendSifCommand(commandId, packet.data(), packetSize))
            {
                // A command without an EE handler is still a completed DMA on  real hardware. Only malformed packets fail above.
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            // TRIAGE RPC-SERVER 2026-10-05 (temporal, visual-primero): ver
            // qué sids registran servidor (¿0x573 de SD_CALL?). Una vez c/u.
            {
                static uint32_t s_loggedSids[16] = {};
                static int s_loggedCount = 0;
                bool seen = false;
                for (int i = 0; i < s_loggedCount; ++i)
                    if (s_loggedSids[i] == server.sid) { seen = true; break; }
                if (!seen && s_loggedCount < 16) {
                    s_loggedSids[s_loggedCount++] = server.sid;
                    std::cout << "[triage-rpc] RegisterRpc sid=0x" << std::hex << server.sid
                              << " func=0x" << server.function << std::dec << std::endl;
                }
            }
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 18:
            setV0(0);
            return true;
        case 19: // SetRpcQueue
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23:
            setV0(0);
            return true;
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};

        // 2026-10-09 (plataforma): servidores del IOP BIOS (IOPRP300) que el
        // recomp no carga. Se responden en HLE para que el bind/call del EE
        // prospere en vez de quedar "unhandled". Reply medido contra ss01
        // (decisions.md 6).
        if (isIopBiosSid(request.sid))
        {
            uint32_t reply[4] = {1u, 0x873280u, 0x8001F930u, 0x10u};
            if (request.sid == 0x8000059cu)
            {
                reply[0] = 2u;
                reply[1] = request.receive.address;
            }
            else if (request.sid == 0x80000006u)
            {
                reply[0] = 0x20u;
                reply[1] = 2u;
            }
            if (request.receive.address != 0u && request.receive.size != 0u)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, sizeof(reply));
                (void)m_host.writeGuest(request.receive.address, reply, copySize);
            }
            result.handled = true;
            result.resultAddress = request.receive.address;
            result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
            result.signalNowaitCompletion = true;
            result.signalCompletion = true;
            return result;
        }

        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        // TRIAGE RPC-DATA 2026-10-06 (temporal, visual-primero): qué recibe
        // el servidor 0x573 por transfer. 2026-10-06b: volcado COMPLETO del
        // comando (4 words = 16 B) + detección de cambios. Sin esto no se
        // puede implementar el bulk-serve de SD.BIN (tarea 4.6-4).
        if (request.sid == 0x573u && request.function == 0x30000u)
        {
            static int s_rpcLog = 0;
            uint32_t w[4] = {};
            uint8_t tmp[16] = {};
            if (request.send.size >= 16 &&
                m_host.readGuest(request.send.address, tmp, sizeof(tmp)))
                std::memcpy(w, tmp, sizeof(w));
            static uint32_t s_last[4] = {};
            bool changed = (w[0] != s_last[0] || w[1] != s_last[1] ||
                            w[2] != s_last[2] || w[3] != s_last[3]);
            if (++s_rpcLog <= 5 || changed)
            {
                s_last[0] = w[0]; s_last[1] = w[1];
                s_last[2] = w[2]; s_last[3] = w[3];
                std::cout << "[triage-rpc] sid=0x573 rpc=0x30000 send=(" << request.send.address << "," << request.send.size << ")"
                          << " cmd=0x" << std::hex << w[0] << " 0x" << w[1] << " 0x" << w[2] << " 0x" << w[3] << std::dec
                          << " serverBuf=0x" << std::hex << serverIt->second.buffer << std::dec
                          << " n=" << s_rpcLog << (changed ? " CHANGED" : "") << std::endl;
            }
        }
        else if (request.sid == 0x573u)
        {
            static int s_rpcLog = 0;
            if (++s_rpcLog <= 3 || s_rpcLog % 100 == 1)
            {
                uint32_t w0 = 0, w1 = 0;
                uint8_t tmp[8] = {};
                if (request.send.size >= 8 &&
                    m_host.readGuest(request.send.address, tmp, sizeof(tmp)))
                {
                    std::memcpy(&w0, tmp, 4);
                    std::memcpy(&w1, tmp + 4, 4);
                }
                std::cout << "[triage-rpc] sid=0x573 rpc=" << request.function
                          << " send=(" << request.send.address << "," << request.send.size << ")"
                          << " w0=0x" << std::hex << w0 << " w1=0x" << w1 << std::dec
                          << " n=" << s_rpcLog << std::endl;
            }
        }

        RpcServer &server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        uint32_t returnPointer = executor.executeGuestFunction(server.function,
                                                               request.function,
                                                               server.buffer,
                                                               request.send.size,
                                                               0u,
                                                               server.gp);
        // 2026-10-06c (desbloqueo): ¿el handler 0x573 tocó el buffer?
        // Si el payload copiado sigue intacto y returnPointer == buffer,
        // el handler no hizo nada (tomó la rama "busy"/default).
        if (request.sid == 0x573u && request.function == 0x30000u)
        {
            static int s_retLog = 0;
            if (++s_retLog <= 3)
            {
                uint32_t b[4] = {};
                for (int i = 0; i < 4; ++i)
                    m_memory.readRam(server.buffer + i * 4u, &b[i], 4u);
                // 2026-10-06d: volcar la STRUCT de reply (ret=0x534D0,
                // buffer+0x400) que el handler devuelve. Si son ceros, el
                // handler tomó la rama busy sin hacer nada.
                // 2026-10-06j: ADEMÁS peek del global de helper#1 (IOP
                // 0xC560, absoluto sin reloc): 0 = rama éxito (mueve datos
                // buffer→tabla), !=0 = rama busy. Sin este dato no se sabe
                // ni qué rama toma.
                uint32_t r[8] = {};
                for (int i = 0; i < 8; ++i)
                    m_memory.readRam(returnPointer + i * 4u, &r[i], 4u);
                uint32_t g_c560 = 0xDEADBEEFu, g_c0fe = 0xDEADBEEFu;
                m_memory.readRam(0xC560u, &g_c560, 4u);
                uint8_t c0fe = 0xEE;
                m_memory.readRam(0xC0FEu, &c0fe, 1u);
                g_c0fe = c0fe;
                std::cout << "[triage-rpc] sid=0x573 rpc=0x30000 ret=0x" << std::hex << returnPointer
                          << " buf=0x" << server.buffer
                          << " b0=0x" << b[0] << " b1=0x" << b[1] << " b2=0x" << b[2] << " b3=0x" << b[3]
                          << " | reply=" << r[0] << " " << r[1] << " " << r[2] << " " << r[3]
                          << " " << r[4] << " " << r[5] << " " << r[6] << " " << r[7]
                          << " | gC560=0x" << g_c560 << " stC0FE=0x" << g_c0fe
                          << std::dec << " n=" << s_retLog << std::endl;
            }
        }
        if (returnPointer == 0u)
            returnPointer = server.buffer;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        if (isIopBiosSid(sid))
            return true;
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
