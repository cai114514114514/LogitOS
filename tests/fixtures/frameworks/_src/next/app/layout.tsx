import type { Metadata } from "next";
export const metadata: Metadata = { title: "next fixture", description: "framework corpus" };
export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (<html lang="en"><body>{children}</body></html>);
}
