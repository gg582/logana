import "./globals.css";
import type { ReactNode } from "react";
import { Space_Grotesk, IBM_Plex_Sans_KR, Fira_Code } from "next/font/google";

const spaceGrotesk = Space_Grotesk({
  subsets: ["latin"],
  variable: "--font-space",
  display: "swap",
});

const ibmPlexSansKr = IBM_Plex_Sans_KR({
  weight: ["400", "500", "600", "700"],
  subsets: ["latin"],
  variable: "--font-ibm-plex-kr",
  display: "swap",
});

const firaCode = Fira_Code({
  subsets: ["latin"],
  variable: "--font-fira",
  display: "swap",
});

export const metadata = {
  title: "Logana",
  description: "High-performance log analytics engine with libttak-backed batching and async rendering",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en">
      <body className={`${spaceGrotesk.variable} ${ibmPlexSansKr.variable} ${firaCode.variable}`}>
        {children}
      </body>
    </html>
  );
}
