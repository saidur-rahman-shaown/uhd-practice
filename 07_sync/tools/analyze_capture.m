function analyze_capture(cap_path, zc_length, zc_root)
%ANALYZE_CAPTURE  Correlate an rx_capture file against a Zadoff-Chu sequence.
%
%   analyze_capture                                % cap.fc32 in the current folder
%   analyze_capture('cap.fc32')
%   analyze_capture('cap.fc32', 401, 25)           % length, root
%   analyze_capture('~/captures/cap.fc32')         % ~ is expanded
%
% Defaults to cap.fc32 in the current folder, so the usual thing is to cd to
% wherever the capture is and just run it.
%
% The sequence is generated here rather than read from disk, so this needs
% only the capture -- nothing has to be copied from the transmitting machine.
% Pass the length and root the transmitter used; they default to the values in
% zc_transmit.
%
% The capture is raw interleaved complex float32, which is what UHD produces
% for the "fc32" host format. Its sidecar .txt carries the sample rate.
%
% What to look for: the peak spacing should equal the sequence length. A tall
% peak at an arbitrary offset is the stream-startup transient, which
% correlates against anything; a peak every N samples is the sequence.

    if nargin < 1 || isempty(cap_path),  cap_path  = 'cap.fc32'; end
    if nargin < 2 || isempty(zc_length), zc_length = 401;                   end
    if nargin < 3 || isempty(zc_root),   zc_root   = 25;                    end

    cap_path = expand_tilde(cap_path);

    meta = read_sidecar(cap_path);
    fs   = getfield_default(meta, 'rate_hz', 30.72e6);

    x  = read_fc32(cap_path);
    zc = zadoff_chu(zc_length, zc_root);
    N  = numel(zc);

    fprintf('  capture   : %d samples, %.1f ms at %.3f MS/s\n', ...
            numel(x), numel(x)/fs*1e3, fs/1e6);
    fprintf('  reference : generated, length %d, root %d\n', N, zc_root);
    fprintf('  rms       : %.6f   (0.00061 = noise, 0.028 = ZC signal)\n', ...
            sqrt(mean(abs(x).^2)));

    % ---------------------------------------------------------------
    % If the transmitter's reference is sitting next to the capture,
    % check the generated sequence against it.
    %
    % Generating it locally is convenient but silently wrong if the
    % length or root do not match what was transmitted. Measured on a
    % real capture of length 401 root 25: correlating with root 26
    % gives peak/mean 1.4, which reads as a dead radio, while length
    % 400 gives 11.9 -- over the detection threshold, with a spacing
    % check that agrees with itself. That one would be believed.
    % ---------------------------------------------------------------
    ref_file = fullfile(fileparts(cap_path), 'zc_reference.fc32');

    if isfile(ref_file)
        ref = read_fc32(ref_file);

        if numel(ref) ~= N
            warning('analyze:lengthMismatch', ...
                ['reference on disk is %d samples but %d was generated -- ' ...
                 'the transmitter used a different zc_length'], numel(ref), N);
        elseif max(abs(ref - zc)) > 1e-4
            warning('analyze:refMismatch', ...
                ['generated sequence differs from the reference on disk ' ...
                 '(max %.2e) -- check zc_root'], max(abs(ref - zc)));
        else
            fprintf('  reference : cross-checked against %s\n', ref_file);
        end
    end

    if ~strcmp(getfield_default(meta, 'overflows', '0'), '0')
        fprintf('  WARNING: capture reports %s overflows -- it has gaps\n', ...
                meta.overflows);
    end

    % Correlate over a couple of hundred repetitions. Eight is enough to find
    % the sequence, but the carrier-offset estimate below averages the phase
    % step between repetitions and needs more of them to be worth trusting.
    win = min(numel(x), 200*N);
    seg = x(1:win);

    if numel(seg) < N
        fprintf('  capture is shorter than the sequence\n');
        return
    end

    % Matched filter: conv with the time-reversed conjugate is the same as
    % sliding correlation, and is far faster than a loop.
    c = conv(seg, conj(flipud(zc)), 'valid');
    m = abs(c);

    [peak, idx] = max(m);
    pmr = peak / mean(m);

    fprintf('  peak/mean : %.1f at lag %d\n', pmr, idx-1);

    if pmr < 5
        fprintf('\n  NO detection -- that is the noise floor.\n');
        fprintf(['  Check the transmitter is running, TX gain is 80, and\n' ...
                 '  that the length and root match what was transmitted:\n' ...
                 '  a root off by one gives peak/mean 1.4 on a good capture.\n']);
        return
    end

    % Peak spacing is the real test of a detection.
    thr     = mean(m) + 0.5*(peak - mean(m));
    peaks   = find(m > thr);
    gaps    = diff(peaks);
    gaps    = gaps(gaps > N/2);
    spacing = median(gaps);

    fprintf('  peaks     : %d above threshold\n', numel(peaks));
    fprintf('  spacing   : %.0f samples (expect %d)\n', spacing, N);

    % Carrier offset between the two radios, from the phase advance between
    % consecutive repetitions of the sequence.
    reps = floor((numel(c) - idx) / N);
    if reps >= 2
        k    = idx + (0:reps-1)*N;
        step = mean(c(k(2:end)) .* conj(c(k(1:end-1))));
        cfo  = angle(step) / (2*pi*N/fs);
        fprintf('  CFO       : %+.1f Hz over %d repetitions\n', cfo, reps);
    end

    if abs(spacing - N) <= 2
        fprintf('\n  DETECTED, spacing matches the sequence length\n');
    else
        fprintf('\n  peaks found but spacing is wrong -- see NOTES section 3.3\n');
    end

    figure('Name', 'ZC capture', 'NumberTitle', 'off');

    subplot(2,1,1);
    plot((0:win-1)/fs*1e3, abs(seg));
    xlabel('ms'); ylabel('|x|'); title('Capture envelope'); grid on;

    subplot(2,1,2);
    plot(0:numel(m)-1, m); hold on;
    plot(peaks-1, m(peaks), 'ro');
    xlabel('lag (samples)'); ylabel('|correlation|');
    title(sprintf('Matched filter, peak/mean %.1f', pmr)); grid on;
end


function x = read_fc32(path)
    f = fopen(path, 'r');
    if f < 0, error('cannot open %s', path); end
    raw = fread(f, Inf, 'float32');
    fclose(f);
    x = complex(raw(1:2:end), raw(2:2:end));
end


function meta = read_sidecar(path)
    meta = struct();
    f = fopen([path '.txt'], 'r');
    if f < 0, return; end
    while true
        line = fgetl(f);
        if ~ischar(line), break; end
        parts = strsplit(strtrim(line), '=');
        if numel(parts) == 2
            meta.(matlab.lang.makeValidName(parts{1})) = parts{2};
        end
    end
    fclose(f);
end


function v = getfield_default(s, name, default)
    if isfield(s, name)
        v = str2double(s.(name));
        if isnan(v), v = s.(name); end
    else
        v = default;
    end
end


function p = expand_tilde(p)
%EXPAND_TILDE  fopen does not expand ~, so do it here.
    if startsWith(p, '~')
        if ispc
            home = getenv('USERPROFILE');
        else
            home = getenv('HOME');
        end
        p = fullfile(home, extractAfter(p, 1));
    end
end
