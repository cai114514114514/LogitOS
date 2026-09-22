"""Navigate the actual Preview library, including rows below the fold."""
import re
import time
from qmp_ui import pt


def click_preview_row(ui, serial, origin, probe, index):
    # The old 70 + index * 20 click silently opened another file after the
    # library redesign. Read geometry emitted by the same drawing/hit-test
    # path, but still verify the opened filename in the caller's independent log.
    ui.key("home", settle=0.8)
    for _ in range(8):
        text = open(serial, errors="replace").read()
        current = text[text.rfind("preview: library "):]
        row = re.search(r"preview: row %d (\d+) (\d+) (\d+) (\d+)" % index, current)
        if row:
            x, y, w, h = map(int, row.groups())
            ui.click_at_confirmed(probe, origin[0] + pt(x + w // 2), origin[1] + pt(y + h // 2))
            time.sleep(0.4)
            return
        ui.key("pgdn", settle=0.8)
    raise AssertionError("Preview did not expose row %d after paging" % index)
