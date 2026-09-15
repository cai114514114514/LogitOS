Serve this directory with an ordinary local HTTP server and load it in the guest.
Record URL, source/image hashes, screenshot, serial and actual keyboard/pointer actions.

1. The right link starts outside the viewport. Shift+wheel, plain Right, and the visible bottom track must each bring it into view without navigation. Alt+Left remains history.
2. Click the JavaScript button: pixels and scrollX/pageXOffset must move together. The second button must log two depth=1 scroll events and no NESTED_SCROLL_EVENT.
3. At the right edge, click the actual collapsed space in “json next”. The target must visibly say HORIZONTAL LINK NAVIGATION REACHED.
4. Test caret placement and the select popup while horizontally scrolled; switch tabs and return. Restart with session restoration and verify both axes.
5. Remain on a page for longer than the script wall-time slice, then navigate: FETCH_READY must remain function. This specifically checks the old stale watchdog deadline.

This fixture isolates input and coordinate behavior. It does not replace the QQ and Python real-page acceptance runs or prove all CSSOM hit-testing features.
