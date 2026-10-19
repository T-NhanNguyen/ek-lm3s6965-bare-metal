"""Exact offline lm3s6965evb output policy; never filter retained raw streams."""
NIC_WARNING = 'qemu-system-arm: warning: nic stellaris_enet.0 has no peer\n'


def check_pass_output(output, marker, returncode=0):
    """Only one optional known prefix, one exact marker, and zero exit."""
    if returncode != 0 or output not in (marker, NIC_WARNING + marker):
        raise RuntimeError('unexpected QEMU result: exit=' + str(returncode) +
                           ' output=' + repr(output))
