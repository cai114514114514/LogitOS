import { Component } from '@angular/core';

@Component({
  selector: 'app-lazy',
  standalone: true,
  template: `<p id="lazy">lazy route loaded</p>`,
})
export class Lazy {}
