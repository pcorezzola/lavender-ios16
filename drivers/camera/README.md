# camera

- **Hardware:** the CAMSS of the Snapdragon 660 (the `qcom,cci`,
  `csiphy-v3.5`, `csid-v5.0`, `ispif-v3.0` and `vfe48` nodes of the phone's
  DTB) and the three sensors it wires to it: the rear main one
  (`camera@0`, CSIPHY0/CSID0, CCI master 0), the second rear one
  (`camera@1`, CSIPHY1/CSID1, CCI master 1, with a VAF actuator) and the
  front one (`camera@2`, CSIPHY2/CSID2, CCI master 1). The DTB lists the
  modules MIUI probes for each slot (OV02A10 and S5K5E8, of the ofilm and
  sunny variants); the one this phone carries is read from its chip ID in
  F1.
- **Code:** `lavender_cam.c`, a boot-arg gated platform probe
  (`lvcam=1` read only, `lvcam=2` also powers the CAMSS domain and reads its
  clock muxes and CSIPHY register interfaces). It registers no device node
  and iOS has no client for it yet.
- **Not started:** the CCI (the sensors' I2C), the sensors' power sequence
  and chip IDs, CSIPHY lane configuration, CSID, the VFE write masters that
  put a frame in memory, the MMSS SMMU the VFE writes through, and the
  userland side (a `/dev` node and a client).
- **Notes for the next step:**
  - Every camera clock is in MMCC (`clock-controller@c8c0000`), none in
    GCC. The `bimc_smmu` GDSC of MMCC is not to be written (mainline keeps
    it commented out for hanging the multimedia subsystem).
  - CCI, CSID and VFE each need their own clocks on top of `camss_top`, and
    the CSID sits in the VFE's power domain: their registers are only read
    after bringing those up, never with them off.
  - The sensors' rails are PM660/PM660L LDOs behind SPMI; reuse the path
    the audio driver verified.
  - The board is in `docs/CAM-ROADMAP.md`, with the traps already paid for.

Mirror: `lavender_cam.c` is reconstructed from `xnu/patches/1001-*.patch` and
the following; edit the patch, never this file.
