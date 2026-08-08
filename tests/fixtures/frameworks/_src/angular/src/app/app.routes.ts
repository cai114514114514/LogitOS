import { Routes } from '@angular/router';

// The empty path redirects into the lazy route so the router's chunk loader
// runs at STARTUP. The probe injects no input; a lazy route reachable only by
// a click is a lazy route this corpus would never measure.
export const routes: Routes = [
  { path: 'lazy', loadComponent: () => import('./lazy').then(m => m.Lazy) },
  { path: '', redirectTo: 'lazy', pathMatch: 'full' },
];
