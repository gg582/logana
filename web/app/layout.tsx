import "./globals.css";
import type { ReactNode } from "react";

export const metadata = {
  title: "Logana",
  description: "High-performance log analytics engine with libttak-backed batching and async rendering",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
