export class LocalStore<T> {
    private _value = $state<T>() as T;
    private key = '';

    constructor(key: string, initialValue: T) {
        this.key = key;
        this._value = initialValue;

        const item = localStorage.getItem(key);
        if (item) {
            try {
                this._value = this.deserialize(item);
            } catch (e) {
                console.error(`Error parsing localStorage item for key "${key}":`, e);
                // Fallback to initial value if parsing fails
                this._value = initialValue;
            }
        }

        $effect(() => {
            localStorage.setItem(this.key, this.serialize(this._value as T));
        });
    }

    /** Returns the current value. Read-only access. */
    get value(): T {
        return this._value;
    }

    /** 
     * Sets a new value and triggers localStorage update.
     * @param newValue - The new value to store
     */
    set(newValue: T): void {
        this._value = newValue;
    }

    /** 
     * Updates the value using a function that receives the current value.
     * Useful for partial updates or transformations.
     * @param updater - Function that receives current value and returns new value
     */
    update(updater: (current: T) => T): void {
        this._value = updater(this._value);
    }

    /** 
     * Resets the value to the initial value provided in constructor.
     * Note: This requires storing the initial value.
     */
    reset(initialValue: T): void {
        this._value = initialValue;
    }

    private serialize(value: T): string {
        return JSON.stringify(value);
    }

    private deserialize(item: string): T {
        return JSON.parse(item);
    }
}

export function useLocalStorage<T>(key: string, initialValue: T) {
    return new LocalStore(key, initialValue);
}
