import type { SVGProps } from "react";

type IconProps = SVGProps<SVGSVGElement> & { size?: number };

function icon(paths: React.ReactNode) {
  return function Icon({ size = 16, ...props }: IconProps) {
    return (
      <svg
        width={size}
        height={size}
        viewBox="0 0 16 16"
        fill="none"
        stroke="currentColor"
        strokeWidth={1.5}
        strokeLinecap="round"
        strokeLinejoin="round"
        aria-hidden="true"
        {...props}
      >
        {paths}
      </svg>
    );
  };
}

export const IconOverview = icon(<><rect x="2" y="2" width="5" height="5" rx="1" /><rect x="9" y="2" width="5" height="3" rx="1" /><rect x="9" y="7" width="5" height="7" rx="1" /><rect x="2" y="9" width="5" height="5" rx="1" /></>);
export const IconOrders = icon(<><path d="M5 4h8M5 8h8M5 12h8" /><path d="M2.5 4h.01M2.5 8h.01M2.5 12h.01" strokeWidth={2} /></>);
export const IconRisk = icon(<path d="M8 1.75 2.75 3.5v4.2c0 3 2.2 5.4 5.25 6.55 3.05-1.15 5.25-3.55 5.25-6.55V3.5L8 1.75Z" />);
export const IconReport = icon(<><path d="M2 13.5h12" /><path d="M3.5 10.5 6.5 7l2.5 2 4-5" /></>);
export const IconSystem = icon(<path d="M1.5 8h3l1.75-4.5 3.5 9L11.5 8h3" />);
export const IconBacktests = icon(<><path d="M2.5 8a5.5 5.5 0 1 0 1.6-3.9" /><path d="M2.5 2.5v2.2h2.2" /><path d="M8 5.25V8l2 1.25" /></>);
export const IconSun = icon(<><circle cx="8" cy="8" r="2.75" /><path d="M8 1.5v1.5M8 13v1.5M1.5 8H3M13 8h1.5M3.4 3.4l1.06 1.06M11.54 11.54l1.06 1.06M3.4 12.6l1.06-1.06M11.54 4.46l1.06-1.06" /></>);
export const IconMoon = icon(<path d="M13.25 9.6A5.5 5.5 0 0 1 6.4 2.75a5.5 5.5 0 1 0 6.85 6.85Z" />);
export const IconCheck = icon(<path d="m3.5 8.5 3 3 6-7" />);
export const IconX = icon(<path d="m4 4 8 8M12 4l-8 8" />);
export const IconResize = icon(<><path d="M2.5 8h8" /><path d="m8 5.5 2.5 2.5L8 10.5" /><path d="M13.5 3.5v9" /></>);
export const IconAlert = icon(<><path d="M8 2 1.75 13h12.5L8 2Z" /><path d="M8 6.5v3M8 11.25h.01" /></>);
export const IconInfo = icon(<><circle cx="8" cy="8" r="6.25" /><path d="M8 7.25v3.75M8 5h.01" /></>);
export const IconChevronDown = icon(<path d="m4 6 4 4 4-4" />);
export const IconArrowUp = icon(<path d="M8 13V3M4 7l4-4 4 4" />);
export const IconArrowDown = icon(<path d="M8 3v10M4 9l4 4 4-4" />);
export const IconPlus = icon(<path d="M8 3v10M3 8h10" />);
export const IconStop = icon(<rect x="4" y="4" width="8" height="8" rx="1" />);
export const IconPower = icon(<><path d="M8 1.75v6" /><path d="M4.4 4.1a5.25 5.25 0 1 0 7.2 0" /></>);
export const IconDownload = icon(<><path d="M8 2.5v8M4.5 7 8 10.5 11.5 7" /><path d="M2.5 13.5h11" /></>);
export const IconClose = IconX;
