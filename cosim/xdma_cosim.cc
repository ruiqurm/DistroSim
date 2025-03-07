/*
 * Written by Qiu Qichen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.	
 */

#include <cstdint>
#include "soc/pci/xilinx/xdma_signal.h"
#include "sysc/communication/sc_clock.h"
#include "sysc/datatypes/int/sc_nbdefs.h"
#include "sysc/kernel/sc_module.h"
#include "sysc/kernel/sc_time.h"
#include "sysc/utils/sc_report.h"
#include "tlm-bridges/axis2tlm-bridge.h"
#include "tlm-bridges/tlm2axis-bridge.h"
#include "tlm_core/tlm_2/tlm_generic_payload/tlm_gp.h"
#define SC_INCLUDE_DYNAMIC_PROCESSES

#include <unistd.h>

#include "systemc.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/tlm_quantumkeeper.h"

#include "memory.h"
#include "soc/pci/core/pcie-root-port.h"
#include "soc/pci/xilinx/xdma.h"
#include "tlm-modules/pcie-controller.h"

#include "debugdev.h"
#include "iconnect.h"
#include "trace.h"

#include "remote-port-tlm-pci-ep.h"
#include "remote-port-tlm.h"

#include "VmkBsvTop.h"
#include "verilated.h"

#define PCI_VENDOR_ID_XILINX (0x10ee)
#define PCI_DEVICE_ID_XILINX_XDMA (0x9038)
#define PCI_SUBSYSTEM_ID_XILINX_TEST (0x000A)

#define PCI_CLASS_BASE_NETWORK_CONTROLLER (0x02)

#define KiB (1024)
#define RAM_SIZE (4 * 8 * KiB)

#define NR_MMIO_BAR 6
#define NR_IRQ 0

// You should not change the following setting manually
// Instead, try to use `xdma_signal_generator.py` to generate the following setting
#define XDMA_CHANNEL_NUM 1
#define XDMA_BYPASS_H2C_BRIDGE tlm2axis_bridge<DMA_DATA_WIDTH>
#define XDMA_BYPASS_C2H_BRIDGE axis2tlm_bridge<DMA_DATA_WIDTH>

class xdma_top : public pci_device_base {
 public:
  SC_HAS_PROCESS(xdma_top);

  xilinx_xdma<XDMA_BYPASS_H2C_BRIDGE, XDMA_BYPASS_C2H_BRIDGE> xdma;
  VmkBsvTop* user_logic;
  xdma_signal xdma_signals;

  sc_clock clock_signal;
  sc_clock slow_clock_signal;

  // BARs towards the XDMA
  tlm_utils::simple_initiator_socket<xdma_top> user_bar_init_socket;
  tlm_utils::simple_initiator_socket<xdma_top> cfg_init_socket;

  // XDMA towards PCIe interface (host)
  tlm_utils::simple_target_socket<xdma_top> brdg_dma_tgt_socket;

  explicit xdma_top(const sc_core::sc_module_name& name)
      : pci_device_base(name, NR_MMIO_BAR, NR_IRQ),
        xdma("xdma", XDMA_CHANNEL_NUM),
        xdma_signals("xdma_signals"),
        clock_signal("clock", 10, SC_NS),
        slow_clock_signal("slow_clock", 20, SC_NS),
        user_bar_init_socket("user_bar_init_socket"),
        cfg_init_socket("cfg_init_socket"),
        brdg_dma_tgt_socket("brdg-dma-tgt-socket") {
    //
    // Init user logic
    //
    user_logic = new VmkBsvTop("user_logic");
    xdma_signals.connect_user_logic(user_logic);
    xdma_signals.connect_xdma(xdma);

    // setup clk
    for (int i = 0; i < XDMA_CHANNEL_NUM; i++) {
      xdma.descriptor_bypass_channels[i].dsc_bypass_bridge_h2c.clk(
          slow_clock_signal);
      xdma.descriptor_bypass_channels[i].dsc_bypass_bridge_c2h.clk(
          slow_clock_signal);
      xdma.descriptor_bypass_channels[i].h2c_bridge.clk(slow_clock_signal);
      xdma.descriptor_bypass_channels[i].c2h_bridge.clk(slow_clock_signal);
    }
    xdma.user_bar.clk(slow_clock_signal);
    user_logic->CLK(clock_signal);
    user_logic->CLK_slowClock(slow_clock_signal);

    //
    // XDMA connections
    //
    cfg_init_socket.bind(xdma.config_bar);
    user_bar_init_socket.bind(xdma.user_bar.tgt_socket);

    // Setup DMA forwarding path (xdma.dma -> upstream to host)
    xdma.dmac.bind(brdg_dma_tgt_socket);

    brdg_dma_tgt_socket.register_b_transport(this,
                                             &xdma_top::fwd_dma_b_transport);
  }

  void rstn(sc_signal<bool>& rst_n) {
    xdma.reset();
    user_logic->RST_N(rst_n);
    user_logic->RST_N_slowReset(rst_n);
    for (int i = 0; i < XDMA_CHANNEL_NUM; i++) {
      xdma.descriptor_bypass_channels[i].dsc_bypass_bridge_c2h.resetn(rst_n);
      xdma.descriptor_bypass_channels[i].dsc_bypass_bridge_h2c.resetn(rst_n);
      xdma.descriptor_bypass_channels[i].h2c_bridge.resetn(rst_n);
      xdma.descriptor_bypass_channels[i].c2h_bridge.resetn(rst_n);
    }
    xdma.user_bar.resetn(rst_n);
  }

 private:
  void bar_b_transport(int bar_nr, tlm::tlm_generic_payload& trans,
                       sc_time& delay) override {
    auto src_addr = static_cast<uint64_t>(trans.get_address());
    auto dst_addr = reinterpret_cast<uint64_t>(trans.get_data_ptr());
    auto cmd = trans.get_command();
    auto len = trans.get_data_length();
    printf("visit bar: bar_nr=%d, src_addr=%lx, dst_addr=%lx, cmd=%d, len=%d\n",
           bar_nr, src_addr, dst_addr, cmd, len);
    switch (bar_nr) {
      case XDMA_USER_BAR_ID:
        user_bar_init_socket->b_transport(trans, delay);
        break;
      case XDMA_CONFIG_BAR_ID:
        cfg_init_socket->b_transport(trans, delay);
        break;
      default:
        SC_REPORT_ERROR("xdma_top", "writing to an unimplemented bar");
        trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        break;
    }
  }

  //
  // Forward DMA requests received from the XDMA
  //
  void fwd_dma_b_transport(tlm::tlm_generic_payload& trans, sc_time& delay) {
    dma->b_transport(trans, delay);
  }
};

/// configure the PCIe property
PhysFuncConfig getPhysFuncConfig() {
  PhysFuncConfig cfg;
  PMCapability pm_cap;
  PCIExpressCapability pcie_cap;
  MSIXCapability msix_cap;
  uint32_t bar_flags = PCI_BASE_ADDRESS_MEM_TYPE_32;
  uint32_t table_offset = 0x100 | 4;  // Table offset: 0, BIR: 4
  uint32_t pba = 0x140000 | 4;        // BIR: 4
  uint32_t max_link_width;

  cfg.SetPCIVendorID(PCI_VENDOR_ID_XILINX);
  // XDMA
  cfg.SetPCIDeviceID(0x903f);

  cfg.SetPCIClassProgIF(0);
  cfg.SetPCIClassDevice(0);
  cfg.SetPCIClassBase(PCI_CLASS_BASE_NETWORK_CONTROLLER);

  cfg.SetPCIBAR0(256 * KiB, bar_flags);
  cfg.SetPCIBAR1(256 * KiB, bar_flags);

  cfg.SetPCISubsystemVendorID(PCI_VENDOR_ID_XILINX);
  cfg.SetPCISubsystemID(PCI_SUBSYSTEM_ID_XILINX_TEST);
  cfg.SetPCIExpansionROMBAR(0, 0);

  cfg.AddPCICapability(pm_cap);

  max_link_width = 1 << 4;
  pcie_cap.SetDeviceCapabilities(PCI_EXP_DEVCAP_RBER);
  pcie_cap.SetLinkCapabilities(PCI_EXP_LNKCAP_SLS_2_5GB | max_link_width |
                               PCI_EXP_LNKCAP_ASPM_L0S);
  pcie_cap.SetLinkStatus(PCI_EXP_LNKSTA_CLS_2_5GB | PCI_EXP_LNKSTA_NLW_X1);
  cfg.AddPCICapability(pcie_cap);

  msix_cap.SetMessageControl(0);
  msix_cap.SetTableOffsetBIR(table_offset);
  msix_cap.SetPendingBitArray(pba);
  cfg.AddPCICapability(msix_cap);

  return cfg;
}

// Host / PCIe RC
//
// This pcie_host uses Remote-port to connect to a QEMU PCIe RC.
// If you'd like to connect this demo to something else, you need
// to replace this implementation with the host model you've got.
//
SC_MODULE(pcie_host) {
 private:
  remoteport_tlm_pci_ep rp_pci_ep_;

 public:
  pcie_root_port rootport;
  sc_in<bool> rst;

  pcie_host(const sc_module_name& name, const char* sk_descr)
      : sc_module(name),
        rp_pci_ep_("rp-pci-ep", 0, 1, 0, sk_descr),
        rootport("rootport"),
        rst("rst") {
    rp_pci_ep_.rst(rst);
    rp_pci_ep_.bind(rootport);
  }
};

SC_MODULE(Top) {
 public:
  SC_HAS_PROCESS(Top);

  pcie_host host;

  PCIeController pcie_ctlr;
  xdma_top xdma;

  sc_signal<bool> rst;
  sc_signal<bool> rst_n;

  Top(const sc_module_name& name, const char* sk_descr, const sc_time& quantum)
      : sc_module(name),
        host("host", sk_descr),
        pcie_ctlr("pcie-ctlr", getPhysFuncConfig()),
        xdma("pcie-xdma"),
        rst("rst") {
    tlm_utils::tlm_quantumkeeper::set_global_quantum(quantum);

    // Setup TLP sockets (host.rootport <-> pcie-ctlr)
    host.rootport.init_socket.bind(pcie_ctlr.tgt_socket);
    pcie_ctlr.init_socket.bind(host.rootport.tgt_socket);

    //
    // PCIeController <-> XDMA connections
    //
    pcie_ctlr.bind(xdma);

    // Reset signal
    host.rst(rst);
    xdma.rstn(rst_n);
    SC_METHOD(invert_reset);
    sensitive << rst;
    SC_THREAD(pull_reset);
  }
  void invert_reset() {
    rst_n.write(!rst.read());
  }
  void pull_reset() {
    /* Pull the reset signal.  */
    rst.write(true);
    wait(1, SC_US);
    rst.write(false);
  }

 private:
  tlm_utils::tlm_quantumkeeper m_qk_;
};

void usage() {
  cout << "tlm socket-path sync-quantum-ns" << endl;
}

int sc_main(int argc, char* argv[]) {
  uint64_t sync_quantum;

  Verilated::commandArgs(argc, argv);

  if (argc < 3) {
    sync_quantum = 10000;
  } else {
    sync_quantum = strtoull(argv[2], nullptr, 10);
  }
  sc_set_time_resolution(1, SC_PS);

  new Top("top", argv[1],
                sc_time(static_cast<double>(sync_quantum), SC_NS));

  if (argc < 3) {
    sc_start(1, SC_PS);
    sc_stop();
    usage();
    exit(EXIT_FAILURE);
  }

  sc_start();

  return 0;
}
