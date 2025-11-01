<svg
  class="logo-icon"
  viewBox="0 0 64 64"
  fill="none"
  xmlns="http://www.w3.org/2000/svg"
>
  <rect
    x="8"
    y="4"
    width="48"
    height="56"
    rx="4"
    fill="url(#grad1)"
    opacity="0.2"
  />
  <rect
    x="12"
    y="8"
    width="40"
    height="48"
    rx="3"
    stroke="url(#grad2)"
    stroke-width="2"
  />
  <circle cx="32" cy="28" r="10" fill="url(#grad3)" />
  <path
    d="M38 28L28 28M32 24L32 32"
    stroke="white"
    stroke-width="2.5"
    stroke-linecap="round"
  />
  <line
    x1="18"
    y1="44"
    x2="46"
    y2="44"
    stroke="url(#grad2)"
    stroke-width="2"
    stroke-linecap="round"
    opacity="0.6"
  />
  <line
    x1="18"
    y1="50"
    x2="38"
    y2="50"
    stroke="url(#grad2)"
    stroke-width="2"
    stroke-linecap="round"
    opacity="0.4"
  />
  <defs>
    <linearGradient
      id="grad1"
      x1="8"
      y1="4"
      x2="56"
      y2="60"
      gradientUnits="userSpaceOnUse"
    >
      <stop stop-color="#60a5fa" />
      <stop offset="1" stop-color="#a78bfa" />
    </linearGradient>
    <linearGradient
      id="grad2"
      x1="12"
      y1="8"
      x2="52"
      y2="56"
      gradientUnits="userSpaceOnUse"
    >
      <stop stop-color="#60a5fa" />
      <stop offset="1" stop-color="#a78bfa" />
    </linearGradient>
    <linearGradient
      id="grad3"
      x1="22"
      y1="18"
      x2="42"
      y2="38"
      gradientUnits="userSpaceOnUse"
    >
      <stop stop-color="#60a5fa" />
      <stop offset="1" stop-color="#8b5cf6" />
    </linearGradient>
  </defs>
</svg>

<style>
  .logo-icon {
    width: 64px;
    height: 64px;
    flex-shrink: 0;
    filter: drop-shadow(0 4px 12px rgba(96, 165, 250, 0.25));
  }

  @media (max-width: 640px) {
    .logo-icon {
      width: 48px;
      height: 48px;
    }
  }

  @media (max-width: 480px) {
    .logo-icon {
      width: 56px;
      height: 56px;
    }
  }
</style>
