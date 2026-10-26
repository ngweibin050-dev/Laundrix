import React, { useState, useRef, useEffect } from "react";
import {
  View,
  Text,
  StyleSheet,
  ScrollView,
  Pressable,
  TextInput,
  KeyboardAvoidingView,
  Platform,
  ActivityIndicator,
  Animated,
  StatusBar,
  Dimensions,
  Keyboard,
} from "react-native";
import { Ionicons } from "@expo/vector-icons";
import { SafeAreaView } from "react-native-safe-area-context";
import { router } from "expo-router";
import { useI18n } from "@/i18n/i18n";
import { LinearGradient } from "expo-linear-gradient";

const { width } = Dimensions.get("window");

// Your Cloudflare Worker URL - API key is securely stored on the backend
const WORKER_URL = "https://gemini-ai.laundrix-gemini-assistant.workers.dev";

interface Message {
  id: string;
  text: string;
  isUser: boolean;
  timestamp: Date;
}

export default function AIAssistant() {
  const { t } = useI18n();
  const [messages, setMessages] = useState<Message[]>([
    {
      id: "0",
      text: t.aiWelcomeMessage,
      isUser: false,
      timestamp: new Date(),
    },
  ]);
  const [inputText, setInputText] = useState("");
  const [loading, setLoading] = useState(false);
  const scrollViewRef = useRef<ScrollView>(null);

  const fadeAnim = useRef(new Animated.Value(0)).current;
  const slideAnim = useRef(new Animated.Value(30)).current;
  const pulseAnim = useRef(new Animated.Value(1)).current;

  useEffect(() => {
    Animated.parallel([
      Animated.timing(fadeAnim, { toValue: 1, duration: 600, useNativeDriver: true }),
      Animated.spring(slideAnim, { toValue: 0, tension: 50, friction: 8, useNativeDriver: true }),
    ]).start();

    Animated.loop(
      Animated.sequence([
        Animated.timing(pulseAnim, { toValue: 1.03, duration: 2000, useNativeDriver: true }),
        Animated.timing(pulseAnim, { toValue: 1, duration: 2000, useNativeDriver: true }),
      ])
    ).start();
  }, []);

  const getAIResponse = async (userPrompt: string): Promise<string> => {
    try {
      // Build conversation history from previous messages (excluding the welcome message).
      // Gemini expects alternating user/model turns; sending only model turns made it
      // lose track of what the student had asked earlier.
      const history = messages
        .filter(m => m.id !== "0")
        .map(m => ({
          role: m.isUser ? ("user" as const) : ("model" as const),
          parts: [{ text: m.text }]
        }));

      const response = await fetch(WORKER_URL, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          message: userPrompt,
          history: history
        }),
      });

      if (!response.ok) {
        throw new Error(`Worker error: ${response.status}`);
      }

      const data = await response.json();
      return data.text || t.aiErrorResponse;
    } catch (error) {
      console.error("AI Error:", error);
      return t.aiConnectionError;
    }
  };

  const sendMessage = async () => {
    if (!inputText.trim() || loading) return;

    Keyboard.dismiss();

    const userMessage: Message = {
      id: Date.now().toString(),
      text: inputText.trim(),
      isUser: true,
      timestamp: new Date(),
    };

    setMessages((prev) => [...prev, userMessage]);
    setInputText("");
    setLoading(true);

    setTimeout(() => scrollViewRef.current?.scrollToEnd({ animated: true }), 100);

    try {
      const aiResponse = await getAIResponse(userMessage.text);
      setMessages((prev) => [...prev, {
        id: (Date.now() + 1).toString(),
        text: aiResponse,
        isUser: false,
        timestamp: new Date(),
      }]);
      setTimeout(() => scrollViewRef.current?.scrollToEnd({ animated: true }), 100);
    } catch (error) {
      console.error("Error:", error);
    } finally {
      setLoading(false);
    }
  };

  return (
    <SafeAreaView style={styles.safe}>
      <StatusBar barStyle="dark-content" />

      {/* Background Decor */}
      <View style={styles.backgroundDecor}>
        <Animated.View style={[styles.decorCircle1, { transform: [{ scale: pulseAnim }] }]} />
        <Animated.View style={[styles.decorCircle2, { transform: [{ scale: pulseAnim }] }]} />
        <View style={styles.decorCircle3} />
      </View>

      <KeyboardAvoidingView 
        behavior={Platform.OS === "ios" ? "padding" : "height"} 
        style={styles.container}
        keyboardVerticalOffset={Platform.OS === "ios" ? 0 : 0}
      >
        <Animated.View style={[styles.content, { opacity: fadeAnim, transform: [{ translateY: slideAnim }] }]}>
          
          {/* Header */}
          <View style={styles.header}>
            <Pressable onPress={() => router.back()} style={styles.backButton}>
              <LinearGradient colors={["#F5F3FF", "#EDE9FE"]} style={styles.backButtonGradient}>
                <Ionicons name="chevron-back" size={24} color="#7C3AED" />
              </LinearGradient>
            </Pressable>
            <View style={styles.headerContent}>
              <View style={styles.headerTitleRow}>
                <Animated.View style={[styles.aiIconGradient, { transform: [{ scale: pulseAnim }] }]}>
                  <LinearGradient colors={["#0284C7", "#7C3AED"]} style={styles.aiIconInner}>
                    <Ionicons name="sparkles" size={20} color="#fff" />
                  </LinearGradient>
                </Animated.View>
                <View>
                  <Text style={styles.headerTitle}>{t.aiHeaderTitle}</Text>
                  <Text style={styles.headerSubtitle}>{t.aiHeaderSubtitle}</Text>
                </View>
              </View>
            </View>
            <View style={styles.headerPlaceholder} />
          </View>

          {/* Messages */}
          <ScrollView
            ref={scrollViewRef}
            style={styles.messagesContainer}
            contentContainerStyle={styles.messagesContent}
            showsVerticalScrollIndicator={false}
            keyboardDismissMode="interactive"
            keyboardShouldPersistTaps="handled"
          >
            {messages.map((message) => (
              <MessageBubble key={message.id} message={message} />
            ))}
            {loading && (
              <View style={styles.loadingContainer}>
                <View style={styles.loadingBubble}>
                  <ActivityIndicator size="small" color="#0284C7" />
                  <Text style={styles.loadingText}>{t.thinking}</Text>
                </View>
              </View>
            )}
          </ScrollView>

          {/* Input Area */}
          <View style={styles.inputContainer}>
            <LinearGradient colors={["#FFFFFF", "#FAFAFA"]} style={styles.inputWrapper}>
              <TextInput
                style={styles.input}
                placeholder={t.aiPlaceholder}
                placeholderTextColor="#94a3b8"
                value={inputText}
                onChangeText={setInputText}
                multiline
                maxLength={500}
                editable={!loading}
              />
              <Pressable
                onPress={sendMessage}
                disabled={!inputText.trim() || loading}
                style={({ pressed }) => [styles.sendButton, pressed && { transform: [{ scale: 0.95 }] }]}
              >
                <LinearGradient
                  colors={!inputText.trim() || loading ? ["#e2e8f0", "#cbd5e1"] : ["#0284C7", "#7C3AED"]}
                  style={styles.sendButtonGradient}
                >
                  <Ionicons name="send" size={18} color={!inputText.trim() || loading ? "#94a3b8" : "#fff"} />
                </LinearGradient>
              </Pressable>
            </LinearGradient>
          </View>
        </Animated.View>
      </KeyboardAvoidingView>
    </SafeAreaView>
  );
}

/* ---------- MESSAGE BUBBLE ---------- */
function MessageBubble({ message }: { message: Message }) {
  return (
    <View style={[styles.messageBubble, message.isUser ? styles.userBubble : styles.aiBubble]}>
      {!message.isUser && (
        <View style={styles.aiAvatarContainer}>
          <LinearGradient colors={["#0284C7", "#7C3AED"]} style={styles.aiAvatar}>
            <Ionicons name="sparkles" size={14} color="#ffffff" />
          </LinearGradient>
        </View>
      )}
      <View style={[styles.bubbleContent, message.isUser ? styles.userBubbleContent : styles.aiBubbleContent]}>
        <Text style={[styles.messageText, message.isUser ? styles.userMessageText : styles.aiMessageText]}>
          {message.text}
        </Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: "#fff" },
  backgroundDecor: { position: "absolute", width: "100%", height: "100%" },
  decorCircle1: {
    position: "absolute",
    width: 250,
    height: 250,
    borderRadius: 125,
    backgroundColor: "#F5F3FF",
    opacity: 0.5,
    top: -80,
    right: -80,
  },
  decorCircle2: {
    position: "absolute",
    width: 180,
    height: 180,
    borderRadius: 90,
    backgroundColor: "#EDE9FE",
    opacity: 0.4,
    bottom: 120,
    left: -50,
  },
  decorCircle3: {
    position: "absolute",
    width: 120,
    height: 120,
    borderRadius: 60,
    backgroundColor: "#DDD6FE",
    opacity: 0.25,
    top: "35%",
    right: -30,
  },
  container: { flex: 1 },
  content: { flex: 1 },
  header: {
    flexDirection: "row",
    alignItems: "center",
    paddingHorizontal: 16,
    paddingVertical: 14,
    borderBottomWidth: 1,
    borderBottomColor: "#f1f5f9",
  },
  backButton: { borderRadius: 14, overflow: "hidden" },
  backButtonGradient: { width: 44, height: 44, alignItems: "center", justifyContent: "center" },
  headerContent: { flex: 1, marginLeft: 12 },
  headerTitleRow: { flexDirection: "row", alignItems: "center", gap: 12 },
  aiIconGradient: {
    width: 44,
    height: 44,
    borderRadius: 14,
    backgroundColor: "#F5F3FF",
    padding: 2,
    elevation: 4,
    shadowColor: "#0284C7",
    shadowOffset: { width: 0, height: 2 },
    shadowOpacity: 0.25,
    shadowRadius: 6,
  },
  aiIconInner: {
    width: 40,
    height: 40,
    borderRadius: 12,
    alignItems: "center",
    justifyContent: "center",
  },
  headerTitle: { fontSize: 18, fontWeight: "800", color: "#0f172a" },
  headerSubtitle: { fontSize: 12, color: "#0284C7", fontWeight: "600", marginTop: 1 },
  headerPlaceholder: { width: 44 },
  messagesContainer: { flex: 1 },
  messagesContent: { padding: 16 },
  messageBubble: { marginBottom: 14, flexDirection: "row", alignItems: "flex-start" },
  userBubble: { justifyContent: "flex-end" },
  aiBubble: { justifyContent: "flex-start" },
  aiAvatarContainer: { marginRight: 10 },
  aiAvatar: {
    width: 32,
    height: 32,
    borderRadius: 10,
    alignItems: "center",
    justifyContent: "center",
    elevation: 2,
    shadowColor: "#0284C7",
    shadowOffset: { width: 0, height: 1 },
    shadowOpacity: 0.2,
    shadowRadius: 3,
  },
  bubbleContent: {
    maxWidth: "78%",
    borderRadius: 20,
    padding: 14,
    elevation: 1,
    shadowColor: "#000",
    shadowOffset: { width: 0, height: 1 },
    shadowOpacity: 0.04,
    shadowRadius: 3,
  },
  userBubbleContent: {
    backgroundColor: "#0284C7",
    borderBottomRightRadius: 6,
  },
  aiBubbleContent: {
    backgroundColor: "#f8fafc",
    borderBottomLeftRadius: 6,
    borderWidth: 1,
    borderColor: "#e2e8f0",
  },
  messageText: { fontSize: 15, lineHeight: 22, fontWeight: "500" },
  userMessageText: { color: "#fff" },
  aiMessageText: { color: "#1e293b" },
  loadingContainer: { flexDirection: "row", alignItems: "center", marginBottom: 14 },
  loadingBubble: {
    flexDirection: "row",
    alignItems: "center",
    backgroundColor: "#f8fafc",
    borderRadius: 18,
    padding: 14,
    gap: 10,
    borderWidth: 1,
    borderColor: "#e2e8f0",
    marginLeft: 42,
  },
  loadingText: { fontSize: 14, color: "#0284C7", fontWeight: "600" },
  inputContainer: {
    paddingHorizontal: 16,
    paddingVertical: 12,
    borderTopWidth: 1,
    borderTopColor: "#f1f5f9",
  },
  inputWrapper: {
    flexDirection: "row",
    alignItems: "flex-end",
    borderRadius: 26,
    paddingHorizontal: 16,
    paddingVertical: 8,
    gap: 12,
    borderWidth: 2,
    borderColor: "#EDE9FE",
    elevation: 4,
    shadowColor: "#0284C7",
    shadowOffset: { width: 0, height: 2 },
    shadowOpacity: 0.08,
    shadowRadius: 8,
  },
  input: {
    flex: 1,
    fontSize: 15,
    color: "#1e293b",
    maxHeight: 100,
    paddingVertical: 8,
    fontWeight: "500",
  },
  sendButton: { borderRadius: 22, overflow: "hidden" },
  sendButtonGradient: {
    width: 44,
    height: 44,
    borderRadius: 22,
    alignItems: "center",
    justifyContent: "center",
  },
});