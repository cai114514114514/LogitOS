/* attached from a <script defer src=...>: the element exists by the time this runs */
document.getElementById('deferred').addEventListener('click', function () { console.log('CLK-deferred'); });
console.log('CLK-DEFERRED-RAN');
