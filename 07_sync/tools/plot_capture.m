function plot_capture(cap_path, n_show, N, u)
%PLOT_CAPTURE  Plot an rx_capture file and correlate it against Zadoff-Chu.
%
%   plot_capture                              % cap.fc32 in the current folder
%   plot_capture('cap.fc32')
%   plot_capture('cap.fc32', 8000)            % samples to plot
%   plot_capture('cap.fc32', 4000, 401, 25)   % samples, length, root
%   plot_capture('~/captures/cap.fc32')       % ~ is expanded
%
% Plots the magnitude, real and imaginary parts of the capture, generates the
% same Zadoff-Chu sequence the transmitter sent, correlates, and reports where
% it landed.
%
% The sequence is generated here rather than read from disk, so this works on
% a capture alone. It matches make_zadoff_chu() in the C++ exactly, including
% the modulo reduction of the phase numerator.
%
% The capture is large -- a second at 30.72 MS/s is 30.7 million samples -- so
% only the first n_show are drawn. Correlation uses a longer stretch.

    if nargin < 1 || isempty(cap_path), cap_path = 'cap.fc32'; end
    if nargin < 2 || isempty(n_show),   n_show   = 4000;                  end
    if nargin < 3 || isempty(N),        N        = 401;   end   % zc_length
    if nargin < 4 || isempty(u),        u        = 25;    end   % zc_root

    cap_path = expand_tilde(cap_path);

    meta = read_sidecar(cap_path);
    fs   = getfield_default(meta, 'rate_hz', 30.72e6);

    x = read_fc32(cap_path);

    fprintf('  capture   : %d samples, %.1f ms at %.3f MS/s\n', ...
            numel(x), numel(x)/fs*1e3, fs/1e6);
    fprintf(['  rms       : %.6f   (0.0006 noise, ~0.057 continuous ZC,\n' ...
     '                        ~0.028 at the 25%% duty zc_tx.py sends)\n'], ...
            sqrt(mean(abs(x).^2)));

    n_over = getfield_default(meta, 'overflows', 0);

    if n_over ~= 0
        fprintf('  WARNING: capture reports %d overflows -- it has gaps\n', ...
                n_over);
    end

    % Same sequence the transmitter sent; see zadoff_chu.m.
    zc = zadoff_chu(N, u);

    fprintf('  ZC        : length %d, root %d, |zc| in [%.6f %.6f]\n', ...
            N, u, min(abs(zc)), max(abs(zc)));

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

    % ---------------------------------------------------------------
    % Plot the capture
    % ---------------------------------------------------------------
    m_show = min(n_show, numel(x));
    seg    = x(1:m_show);
    t_us   = (0:m_show-1) / fs * 1e6;

    figure('Name', 'Capture', 'NumberTitle', 'off');

    subplot(3,1,1);
    plot(t_us, abs(seg));
    ylabel('|x|'); title(sprintf('Capture, first %d samples', m_show));
    grid on; xlim([t_us(1) t_us(end)]);

    subplot(3,1,2);
    plot(t_us, real(seg));
    ylabel('real'); grid on; xlim([t_us(1) t_us(end)]);

    subplot(3,1,3);
    plot(t_us, imag(seg));
    ylabel('imag'); xlabel('time (\mus)');
    grid on; xlim([t_us(1) t_us(end)]);

    % ---------------------------------------------------------------
    % Plot the reference
    % ---------------------------------------------------------------
    figure('Name', 'Zadoff-Chu reference', 'NumberTitle', 'off');

    subplot(2,1,1);
    plot(0:N-1, real(zc), 0:N-1, imag(zc));
    legend('real', 'imag', 'Location', 'best');
    title(sprintf('Zadoff-Chu, length %d, root %d', N, u));
    xlabel('sample'); grid on; xlim([0 N-1]);

    subplot(2,1,2);
    plot(0:N-1, abs(zc));
    ylabel('|zc|'); xlabel('sample');
    title('Constant modulus'); grid on;
    xlim([0 N-1]); ylim([0 1.5]);

    % ---------------------------------------------------------------
    % Correlate. conv against the reversed conjugate is the matched
    % filter, and is far faster than a sliding loop.
    % ---------------------------------------------------------------
    win  = min(numel(x), 200*N);
    corr = conv(x(1:win), conj(flipud(zc)), 'valid');
    mag  = abs(corr);

    [peak, idx] = max(mag);
    pmr = peak / mean(mag);

    fprintf('  peak/mean : %.1f at lag %d\n', pmr, idx-1);

    if pmr < 5
        fprintf('\n  NO detection -- that is the noise floor.\n');
        fprintf(['  Check the transmitter is running, TX gain is 80, and\n' ...
                 '  that the length and root match what was transmitted:\n' ...
                 '  a root off by one gives peak/mean 1.4 on a good capture.\n']);
    else
        thr     = mean(mag) + 0.5*(peak - mean(mag));
        peaks   = find(mag > thr);
        gaps    = diff(peaks);
        gaps    = gaps(gaps > N/2);
        spacing = median(gaps);

        fprintf('  peaks     : %d above threshold\n', numel(peaks));
        fprintf('  spacing   : %.0f samples (expect %d)\n', spacing, N);

        % Carrier offset from the phase advance between repetitions.
        reps = floor((numel(corr) - idx) / N);
        if reps >= 2
            k    = idx + (0:reps-1)*N;
            step = mean(corr(k(2:end)) .* conj(corr(k(1:end-1))));
            cfo  = angle(step) / (2*pi*N/fs);
            fprintf('  CFO       : %+.1f Hz over %d repetitions\n', cfo, reps);
        end

        if abs(spacing - N) <= 2
            fprintf('\n  DETECTED, spacing matches the sequence length\n');
        else
            fprintf('\n  peaks found but spacing is wrong -- see NOTES section 3.3\n');
        end
    end

    figure('Name', 'Correlation', 'NumberTitle', 'off');

    subplot(2,1,1);
    plot(0:numel(mag)-1, mag);
    xlabel('lag (samples)'); ylabel('|correlation|');
    title(sprintf('Matched filter over %d samples, peak/mean %.1f', win, pmr));
    grid on;

    subplot(2,1,2);
    lo = max(1, idx - 3*N);
    hi = min(numel(mag), idx + 3*N);
    plot(lo-1:hi-1, mag(lo:hi));
    xlabel('lag (samples)'); ylabel('|correlation|');
    title('Around the peak — spacing should equal the sequence length');
    grid on;
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
