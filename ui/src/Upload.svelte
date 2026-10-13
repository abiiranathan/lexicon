<!-- Upload.svelte — admin-only chunked PDF upload with indexing progress. -->
<script lang="ts">
  import {
    uploadChunkUrl,
    uploadComplete,
    uploadInit,
    uploadJobStatus,
    type UploadJob,
  } from "./lib/httpclient";
  import type { AuthState } from "./lib/auth.svelte";

  type UploadProps = {
    auth: AuthState;
    onUploaded: () => void;
  };

  let { auth, onUploaded }: UploadProps = $props();

  const CHUNK_SIZE = 4 * 1024 * 1024;
  const POLL_INTERVAL_MS = 2000;

  type Phase = "idle" | "uploading" | "indexing" | "done" | "error";

  let file = $state<File | null>(null);
  let phase = $state<Phase>("idle");
  let progress = $state(0);
  let statusText = $state("");
  let error = $state<string | null>(null);
  let pollTimer: ReturnType<typeof setInterval> | undefined;

  const busy = $derived(phase === "uploading" || phase === "indexing");

  function pickFile(e: Event): void {
    const input = e.currentTarget as HTMLInputElement;
    const chosen = input.files?.[0] ?? null;
    error = null;
    if (!chosen) {
      file = null;
      return;
    }
    if (!/\.pdf$/i.test(chosen.name)) {
      error = "Only PDF files can be uploaded.";
      file = null;
      input.value = "";
      return;
    }
    if (chosen.size < 1) {
      error = "Selected file is empty.";
      file = null;
      input.value = "";
      return;
    }
    file = chosen;
    phase = "idle";
    progress = 0;
    statusText = "";
  }

  function putChunk(url: string, blob: Blob, loaded: number): Promise<void> {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", url);
      xhr.withCredentials = true;
      xhr.upload.onprogress = (e: ProgressEvent) => {
        if (e.lengthComputable) {
          progress = Math.min(
            99,
            Math.round(((loaded + e.loaded) / (file?.size ?? 1)) * 100),
          );
        }
      };
      xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) resolve();
        else reject(new Error(`Chunk upload failed (HTTP ${xhr.status})`));
      };
      xhr.onerror = () => reject(new Error("Chunk upload failed (network error)"));
      xhr.send(blob);
    });
  }

  function stopPolling(): void {
    clearInterval(pollTimer);
    pollTimer = undefined;
  }

  async function pollJob(jobId: number): Promise<void> {
    stopPolling();
    pollTimer = setInterval(async () => {
      try {
        const job: UploadJob = await uploadJobStatus(jobId);
        if (job.status === "done") {
          stopPolling();
          phase = "done";
          progress = 100;
          statusText = `Indexed as “${job.filename}”.`;
          onUploaded();
        } else if (job.status === "failed" || job.status === "aborted") {
          stopPolling();
          phase = "error";
          error = job.error ?? "Indexing failed.";
        } else {
          statusText = `Indexing… (${job.status})`;
        }
      } catch (err) {
        stopPolling();
        phase = "error";
        error = (err as Error).message;
      }
    }, POLL_INTERVAL_MS);
  }

  async function startUpload(): Promise<void> {
    if (!file || busy) return;
    error = null;
    phase = "uploading";
    progress = 0;
    statusText = "Starting upload…";

    try {
      const job: UploadJob = await uploadInit(file.name, file.size);
      let offset = job.received_bytes ?? 0;
      statusText = "Uploading…";
      while (offset < file.size) {
        const slice = file.slice(offset, offset + CHUNK_SIZE);
        await putChunk(uploadChunkUrl(job.job_id, offset), slice, offset);
        offset += slice.size;
        progress = Math.min(
          99,
          Math.round((offset / file.size) * 100),
        );
      }
      statusText = "Finalizing…";
      const finished: UploadJob = await uploadComplete(job.job_id);
      if (finished.status === "failed") {
        phase = "error";
        error = finished.error ?? "Upload failed.";
        return;
      }
      phase = "indexing";
      statusText = "Indexing…";
      await pollJob(job.job_id);
    } catch (err) {
      phase = "error";
      error = (err as Error).message;
    }
  }

  function reset(): void {
    stopPolling();
    file = null;
    phase = "idle";
    progress = 0;
    statusText = "";
    error = null;
  }

  $effect(() => {
    return () => stopPolling();
  });
</script>

{#if auth.user?.is_admin}
  <div class="upload-card">
    <div class="upload-row">
      <label class="upload-pick">
        <input
          type="file"
          accept=".pdf,application/pdf"
          onchange={pickFile}
          disabled={busy}
          aria-label="Choose a PDF to upload"
        />
        <span class="toggle-btn">{file ? "Change file" : "Choose PDF…"}</span>
      </label>
      {#if file}
        <span class="upload-name" title={file.name}>
          {file.name} ({(file.size / 1048576).toFixed(1)} MB)
        </span>
      {/if}
      <button
        type="button"
        class="toggle-btn active"
        disabled={!file || busy}
        onclick={startUpload}
      >
        {phase === "uploading"
          ? "Uploading…"
          : phase === "indexing"
            ? "Indexing…"
            : "Upload"}
      </button>
      {#if phase === "done" || phase === "error"}
        <button type="button" class="toggle-btn" onclick={reset}>Reset</button>
      {/if}
    </div>

    {#if busy || phase === "done"}
      <div class="upload-progress" role="progressbar" aria-valuenow={progress}>
        <div class="upload-progress-fill" style={`width:${progress}%`}></div>
      </div>
      {#if statusText}<p class="upload-status">{statusText}</p>{/if}
    {/if}
    {#if error}<p class="upload-error">{error}</p>{/if}
  </div>
{/if}

<style>
  .upload-card {
    background: rgba(17, 24, 39, 0.5);
    border: 1px solid var(--border);
    border-radius: 1rem;
    padding: 1rem 1.25rem;
    margin-bottom: 1.5rem;
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }

  .upload-row {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    flex-wrap: wrap;
  }

  .upload-pick input[type="file"] {
    display: none;
  }

  .upload-pick .toggle-btn {
    cursor: pointer;
  }

  .upload-name {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--text-secondary);
    font-size: 0.875rem;
  }

  .upload-progress {
    height: 6px;
    border-radius: 3px;
    background: rgba(0, 0, 0, 0.35);
    overflow: hidden;
  }

  .upload-progress-fill {
    height: 100%;
    background: var(--primary);
    transition: width 0.2s ease;
  }

  .upload-status {
    margin: 0;
    font-size: 0.8125rem;
    color: var(--text-secondary);
  }

  .upload-error {
    margin: 0;
    font-size: 0.8125rem;
    color: var(--error);
  }
</style>
