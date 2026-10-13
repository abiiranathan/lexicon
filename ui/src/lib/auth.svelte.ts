// AuthState owns the session: current user, configured providers,
// login (OAuth redirect) and logout.

import {
    authLoginUrl,
    authLogout,
    authProviders,
    authWhoami,
    type AuthProviders,
    type AuthUser,
} from "./httpclient";

export class AuthState {
    user = $state<AuthUser | null>(null);
    providers = $state<AuthProviders>({ google: false, github: false });
    loading = $state(true);

    /** Loads providers + session. Call once on app mount. */
    refresh = async (): Promise<void> => {
        this.loading = true;
        try {
            const [providers, user] = await Promise.all([
                authProviders().catch(() => ({ google: false, github: false })),
                authWhoami().catch(() => null),
            ]);
            this.providers = providers;
            this.user = user;
        } finally {
            this.loading = false;
        }
    };

    login = (provider: "google" | "github"): void => {
        // Full-page redirect into the backend OAuth flow.
        window.location.href = authLoginUrl(provider);
    };

    logout = async (): Promise<void> => {
        try {
            await authLogout();
        } catch {
            // Clearing local state regardless; the cookie is expired server-side
            // on best effort.
        }
        this.user = null;
    };
}
