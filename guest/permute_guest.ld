/* SPDX-License-Identifier: Apache-2.0
 *
 * Version script for a guest process object.
 *
 * Exports `permute_main` and the interposed ABI, and hides everything else.
 * The interposed list is generated from abi/permute_iface.def; run
 * `tools/make_guest.py --check-linker-script` to detect drift.
 */
{
  global:
    permute_main;
    ChannelCreate;
    ConnectAttach;
    MsgSend;
    MsgReceive;
    MsgReply;
  local:
    *;
};
