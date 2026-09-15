"""QEMU topology and native rates for the capture acceptance matrix.

The mixed-card fixture deliberately gives HDA an output-only codec. An HDA
duplex codec would supply the first ADC too, allowing a broken second card's
capture registration to pass unnoticed. Explicit PCI slots preserve probe order.
"""

NATIVE_RATES = {"intel-hda": 48000, "AC97": 48000, "ES1370": 48662}


def device_arguments(capture_device, separate_output=None):
    if capture_device not in NATIVE_RATES:
        raise ValueError("unsupported capture fixture")
    if separate_output is not None:
        if separate_output != "intel-hda" or capture_device == "intel-hda":
            raise ValueError("mixed capture requires HDA output and an AC97/ES1370 input")
        return ["-device", "intel-hda,addr=0x10", "-device", "hda-output,audiodev=snd0",
                "-device", f"{capture_device},addr=0x11,audiodev=snd0"]
    if capture_device == "intel-hda":
        return ["-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0"]
    return ["-device", f"{capture_device},audiodev=snd0"]
